// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/login/managed/supervised_user_authentication.h"

#include "base/base64.h"
#include "base/command_line.h"
#include "base/json/json_file_value_serializer.h"
#include "base/macros.h"
#include "base/metrics/histogram.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/threading/sequenced_worker_pool.h"
#include "chrome/browser/chromeos/login/managed/locally_managed_user_constants.h"
#include "chrome/browser/chromeos/login/supervised_user_manager.h"
#include "chrome/browser/chromeos/login/user.h"
#include "chrome/browser/chromeos/login/user_manager.h"
#include "chrome/browser/chromeos/profiles/profile_helper.h"
#include "chromeos/chromeos_switches.h"
#include "chromeos/cryptohome/signed_secret.pb.h"
#include "content/public/browser/browser_thread.h"
#include "crypto/hmac.h"
#include "crypto/random.h"
#include "crypto/symmetric_key.h"

namespace chromeos {

namespace {

// Byte size of hash salt.
const unsigned kSaltSize = 32;

// Parameters of cryptographic hashing for new user schema.
const unsigned kNumIterations = 1234;
const unsigned kKeySizeInBits = 256;

// Size of key signature.
const unsigned kHMACKeySizeInBits = 256;
const int kSignatureLength = 32;

// Size of master key (in bytes).
const int kMasterKeySize = 32;

std::string CreateSalt() {
    char result[kSaltSize];
    crypto::RandBytes(&result, sizeof(result));
    return StringToLowerASCII(base::HexEncode(
        reinterpret_cast<const void*>(result),
        sizeof(result)));
}

std::string BuildPasswordForHashWithSaltSchema(
  const std::string& salt,
  const std::string& plain_password) {
  scoped_ptr<crypto::SymmetricKey> key(
      crypto::SymmetricKey::DeriveKeyFromPassword(
          crypto::SymmetricKey::AES,
          plain_password, salt,
          kNumIterations, kKeySizeInBits));
  std::string raw_result, result;
  key->GetRawKey(&raw_result);
  base::Base64Encode(raw_result, &result);
  return result;
}

std::string BuildRawHMACKey() {
  scoped_ptr<crypto::SymmetricKey> key(crypto::SymmetricKey::GenerateRandomKey(
      crypto::SymmetricKey::AES, kHMACKeySizeInBits));
  std::string raw_result, result;
  key->GetRawKey(&raw_result);
  base::Base64Encode(raw_result, &result);
  return result;
}

std::string BuildPasswordSignature(const std::string& password,
                                   int revision,
                                   const std::string& base64_signature_key) {
  ac::chrome::managedaccounts::account::Secret secret;
  secret.set_revision(revision);
  secret.set_secret(password);
  std::string buffer;
  if (!secret.SerializeToString(&buffer))
    LOG(FATAL) << "Protobuf::SerializeToString failed";
  std::string signature_key;
  base::Base64Decode(base64_signature_key, &signature_key);

  crypto::HMAC hmac(crypto::HMAC::SHA256);
  if (!hmac.Init(signature_key))
    LOG(FATAL) << "HMAC::Init failed";

  unsigned char out_bytes[kSignatureLength];
  if (!hmac.Sign(buffer, out_bytes, sizeof(out_bytes)))
    LOG(FATAL) << "HMAC::Sign failed";

  std::string raw_result(out_bytes, out_bytes + sizeof(out_bytes));

  std::string result;
  base::Base64Encode(raw_result, &result);
  return result;
}

base::DictionaryValue* LoadPasswordData(base::FilePath profile_dir) {
  JSONFileValueSerializer serializer(profile_dir.Append(kPasswordUpdateFile));
  std::string error_message;
  int error_code = JSONFileValueSerializer::JSON_NO_ERROR;
  scoped_ptr<base::Value> value(
      serializer.Deserialize(&error_code, &error_message));
  if (JSONFileValueSerializer::JSON_NO_ERROR != error_code) {
    LOG(ERROR) << "Could not deserialize password data, error = " << error_code
               << " / " << error_message;
    return NULL;
  }
  base::DictionaryValue* result;
  if (!value->GetAsDictionary(&result)) {
    LOG(ERROR) << "Stored password data is not a dictionary";
    return NULL;
  }
  ignore_result(value.release());
  return result;
}

void OnPasswordDataLoaded(
    const SupervisedUserAuthentication::PasswordDataCallback& success_callback,
    const base::Closure& failure_callback,
    base::DictionaryValue* value) {
  if (!value) {
    failure_callback.Run();
    return;
  }
  success_callback.Run(value);
  delete value;
}

}  // namespace

SupervisedUserAuthentication::SupervisedUserAuthentication(
    SupervisedUserManager* owner)
      : owner_(owner),
        stable_schema_(SCHEMA_PLAIN) {
  CommandLine* command_line = CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kEnableSupervisedPasswordSync)) {
    stable_schema_ = SCHEMA_SALT_HASHED;
  }
}

SupervisedUserAuthentication::~SupervisedUserAuthentication() {}

SupervisedUserAuthentication::Schema
SupervisedUserAuthentication::GetStableSchema() {
  return stable_schema_;
}

std::string SupervisedUserAuthentication::TransformPassword(
    const std::string& user_id,
    const std::string& password) {
  int user_schema = GetPasswordSchema(user_id);
  if (user_schema == SCHEMA_PLAIN)
    return password;

  if (user_schema == SCHEMA_SALT_HASHED) {
    base::DictionaryValue holder;
    std::string salt;
    owner_->GetPasswordInformation(user_id, &holder);
    holder.GetStringWithoutPathExpansion(kSalt, &salt);
    DCHECK(!salt.empty());
    return BuildPasswordForHashWithSaltSchema(salt, password);
  }
  NOTREACHED();
  return password;
}

UserContext SupervisedUserAuthentication::TransformPasswordInContext(
    const UserContext& context) {
  UserContext result;
  result.CopyFrom(context);
  int user_schema = GetPasswordSchema(context.username);
  if (user_schema == SCHEMA_PLAIN)
    return result;

  if (user_schema == SCHEMA_SALT_HASHED) {
    base::DictionaryValue holder;
    std::string salt;
    owner_->GetPasswordInformation(context.username, &holder);
    holder.GetStringWithoutPathExpansion(kSalt, &salt);
    DCHECK(!salt.empty());
    result.password =
        BuildPasswordForHashWithSaltSchema(salt, context.password);
    result.need_password_hashing = false;
    result.using_oauth = false;
    result.key_label = kCryptohomeManagedUserKeyLabel;
    return result;
  }
  NOTREACHED() << "Unknown password schema for " << context.username;
  return context;
}

bool SupervisedUserAuthentication::FillDataForNewUser(
    const std::string& user_id,
    const std::string& password,
    base::DictionaryValue* password_data,
    base::DictionaryValue* extra_data) {
  Schema schema = stable_schema_;
  if (schema == SCHEMA_PLAIN)
    return false;

  if (schema == SCHEMA_SALT_HASHED) {
    password_data->SetIntegerWithoutPathExpansion(kSchemaVersion, schema);
    std::string salt = CreateSalt();
    password_data->SetStringWithoutPathExpansion(kSalt, salt);
    int revision = kMinPasswordRevision;
    password_data->SetIntegerWithoutPathExpansion(kPasswordRevision, revision);
    std::string salted_password =
        BuildPasswordForHashWithSaltSchema(salt, password);
    std::string base64_signature_key = BuildRawHMACKey();
    std::string base64_signature =
        BuildPasswordSignature(salted_password, revision, base64_signature_key);
    password_data->SetStringWithoutPathExpansion(kEncryptedPassword,
                                                 salted_password);
    password_data->SetStringWithoutPathExpansion(kPasswordSignature,
                                                 base64_signature);

    extra_data->SetStringWithoutPathExpansion(kPasswordEncryptionKey,
                                              BuildRawHMACKey());
    extra_data->SetStringWithoutPathExpansion(kPasswordSignatureKey,
                                              base64_signature_key);
    return true;
  }
  NOTREACHED();
  return false;
}

std::string SupervisedUserAuthentication::GenerateMasterKey() {
  char master_key_bytes[kMasterKeySize];
  crypto::RandBytes(&master_key_bytes, sizeof(master_key_bytes));
  return StringToLowerASCII(
      base::HexEncode(reinterpret_cast<const void*>(master_key_bytes),
                      sizeof(master_key_bytes)));
}

void SupervisedUserAuthentication::StorePasswordData(
    const std::string& user_id,
    const base::DictionaryValue& password_data) {
  base::DictionaryValue holder;
  owner_->GetPasswordInformation(user_id, &holder);
  const base::Value* value;
  if (password_data.GetWithoutPathExpansion(kSchemaVersion, &value))
      holder.SetWithoutPathExpansion(kSchemaVersion, value->DeepCopy());
  if (password_data.GetWithoutPathExpansion(kSalt, &value))
      holder.SetWithoutPathExpansion(kSalt, value->DeepCopy());
  if (password_data.GetWithoutPathExpansion(kPasswordRevision, &value))
      holder.SetWithoutPathExpansion(kPasswordRevision, value->DeepCopy());
  owner_->SetPasswordInformation(user_id, &holder);
}

SupervisedUserAuthentication::Schema
SupervisedUserAuthentication::GetPasswordSchema(
  const std::string& user_id) {
  base::DictionaryValue holder;

  owner_->GetPasswordInformation(user_id, &holder);
  // Default version.
  int schema_version_index;
  Schema schema_version = SCHEMA_PLAIN;
  if (holder.GetIntegerWithoutPathExpansion(kSchemaVersion,
                                            &schema_version_index)) {
    schema_version = static_cast<Schema>(schema_version_index);
  }
  return schema_version;
}

bool SupervisedUserAuthentication::NeedPasswordChange(
    const std::string& user_id,
    const base::DictionaryValue* password_data) {
  base::DictionaryValue local;
  owner_->GetPasswordInformation(user_id, &local);
  int local_schema = SCHEMA_PLAIN;
  int local_revision = kMinPasswordRevision;
  int updated_schema = SCHEMA_PLAIN;
  int updated_revision = kMinPasswordRevision;
  local.GetIntegerWithoutPathExpansion(kSchemaVersion, &local_schema);
  local.GetIntegerWithoutPathExpansion(kPasswordRevision, &local_revision);
  password_data->GetIntegerWithoutPathExpansion(kSchemaVersion,
                                                &updated_schema);
  password_data->GetIntegerWithoutPathExpansion(kPasswordRevision,
                                                &updated_revision);
  if (updated_schema > local_schema)
    return true;
  DCHECK_EQ(updated_schema, local_schema);
  return updated_revision > local_revision;
}

void SupervisedUserAuthentication::ScheduleSupervisedPasswordChange(
    const std::string& supervised_user_id,
    const base::DictionaryValue* password_data) {
  const User* user = UserManager::Get()->FindUser(supervised_user_id);
  base::FilePath profile_path = ProfileHelper::GetProfilePathByUserIdHash(
      user->username_hash());
  JSONFileValueSerializer serializer(profile_path.Append(kPasswordUpdateFile));
  if (!serializer.Serialize(*password_data)) {
    LOG(ERROR) << "Failed to schedule password update for supervised user "
               << supervised_user_id;
    UMA_HISTOGRAM_ENUMERATION(
        "ManagedUsers.ChromeOS.PasswordChange",
        SupervisedUserAuthentication::PASSWORD_CHANGE_FAILED_STORE_DATA,
        SupervisedUserAuthentication::PASSWORD_CHANGE_RESULT_MAX_VALUE);
    return;
  }
  base::DictionaryValue holder;
  owner_->GetPasswordInformation(supervised_user_id, &holder);
  holder.SetBoolean(kRequirePasswordUpdate, true);
  owner_->SetPasswordInformation(supervised_user_id, &holder);
}

bool SupervisedUserAuthentication::HasScheduledPasswordUpdate(
    const std::string& user_id) {
  base::DictionaryValue holder;
  owner_->GetPasswordInformation(user_id, &holder);
  bool require_update = false;
  holder.GetBoolean(kRequirePasswordUpdate, &require_update);
  return require_update;
}

void SupervisedUserAuthentication::ClearScheduledPasswordUpdate(
    const std::string& user_id) {
  base::DictionaryValue holder;
  owner_->GetPasswordInformation(user_id, &holder);
  holder.SetBoolean(kRequirePasswordUpdate, false);
  owner_->SetPasswordInformation(user_id, &holder);
}

bool SupervisedUserAuthentication::HasIncompleteKey(
    const std::string& user_id) {
  base::DictionaryValue holder;
  owner_->GetPasswordInformation(user_id, &holder);
  bool incomplete_key = false;
  holder.GetBoolean(kHasIncompleteKey, &incomplete_key);
  return incomplete_key;
}

void SupervisedUserAuthentication::MarkKeyIncomplete(const std::string& user_id,
                                                     bool incomplete) {
  base::DictionaryValue holder;
  owner_->GetPasswordInformation(user_id, &holder);
  holder.SetBoolean(kHasIncompleteKey, incomplete);
  owner_->SetPasswordInformation(user_id, &holder);
}

void SupervisedUserAuthentication::LoadPasswordUpdateData(
    const std::string& user_id,
    const PasswordDataCallback& success_callback,
    const base::Closure& failure_callback) {
  const User* user = UserManager::Get()->FindUser(user_id);
  base::FilePath profile_path =
      ProfileHelper::GetProfilePathByUserIdHash(user->username_hash());
  PostTaskAndReplyWithResult(
      content::BrowserThread::GetBlockingPool(),
      FROM_HERE,
      base::Bind(&LoadPasswordData, profile_path),
      base::Bind(&OnPasswordDataLoaded, success_callback, failure_callback));
}

}  // namespace chromeos
