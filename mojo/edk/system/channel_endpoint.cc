// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/edk/system/channel_endpoint.h"

#include "base/logging.h"
#include "base/threading/platform_thread.h"
#include "mojo/edk/system/channel.h"
#include "mojo/edk/system/channel_endpoint_client.h"

namespace mojo {
namespace system {

ChannelEndpoint::ChannelEndpoint(ChannelEndpointClient* client,
                                 unsigned client_port,
                                 MessageInTransitQueue* message_queue)
    : client_(client),
      client_port_(client_port),
      channel_(nullptr),
      is_detached_from_channel_(false) {
  DCHECK(client_ || message_queue);

  if (message_queue)
    channel_message_queue_.Swap(message_queue);
}

bool ChannelEndpoint::EnqueueMessage(scoped_ptr<MessageInTransit> message) {
  DCHECK(message);

  base::AutoLock locker(lock_);

  if (!channel_) {
    // We may reach here if we haven't been attached/run yet.
    // TODO(vtl): We may also reach here if the channel is shut down early for
    // some reason (with live message pipes on it). Ideally, we'd return false
    // (and not enqueue the message), but we currently don't have a way to check
    // this.
    channel_message_queue_.AddMessage(message.Pass());
    return true;
  }

  return WriteMessageNoLock(message.Pass());
}

bool ChannelEndpoint::ReplaceClient(ChannelEndpointClient* client,
                                    unsigned client_port) {
  DCHECK(client);

  base::AutoLock locker(lock_);
  DCHECK(client_);
  DCHECK(client != client_.get() || client_port != client_port_);
  client_ = client;
  client_port_ = client_port;
  return !is_detached_from_channel_;
}

void ChannelEndpoint::DetachFromClient() {
  base::AutoLock locker(lock_);
  DCHECK(client_);
  client_ = nullptr;

  if (!channel_)
    return;
  channel_->DetachEndpoint(this, local_id_, remote_id_);
  ResetChannelNoLock();
}

void ChannelEndpoint::AttachAndRun(Channel* channel,
                                   ChannelEndpointId local_id,
                                   ChannelEndpointId remote_id) {
  DCHECK(channel);
  DCHECK(local_id.is_valid());
  DCHECK(remote_id.is_valid());

  base::AutoLock locker(lock_);
  DCHECK(!channel_);
  DCHECK(!local_id_.is_valid());
  DCHECK(!remote_id_.is_valid());
  channel_ = channel;
  local_id_ = local_id;
  remote_id_ = remote_id;

  while (!channel_message_queue_.IsEmpty()) {
    LOG_IF(WARNING, !WriteMessageNoLock(channel_message_queue_.GetMessage()))
        << "Failed to write enqueue message to channel";
  }

  if (!client_) {
    channel_->DetachEndpoint(this, local_id_, remote_id_);
    ResetChannelNoLock();
  }
}

void ChannelEndpoint::OnReadMessage(scoped_ptr<MessageInTransit> message) {
  scoped_refptr<ChannelEndpointClient> client;
  unsigned client_port = 0;

  // This loop is to make |ReplaceClient()| work. We can't call the client's
  // |OnReadMessage()| under our lock, so by the time we do that, |client| may
  // no longer be our client.
  //
  // In that case, |client| must return false. We'll then yield, and retry with
  // the new client. (Theoretically, the client could be replaced again.)
  //
  // This solution isn't terribly elegant, but it's the least costly way of
  // handling/avoiding this (very unlikely) race. (Other solutions -- e.g.,
  // adding a client message queue, which the client only fetches messages from
  // -- impose significant cost in the common case.)
  for (;;) {
    {
      base::AutoLock locker(lock_);
      if (!channel_ || !client_) {
        // This isn't a failure per se. (It just means that, e.g., the other end
        // of the message point closed first.)
        return;
      }

      // If we get here in a second (third, etc.) iteration of the loop, it's
      // because |ReplaceClient()| was called.
      DCHECK(client_ != client || client_port_ != client_port);

      // Take a ref, and call |OnReadMessage()| outside the lock.
      client = client_;
      client_port = client_port_;
    }

    if (client->OnReadMessage(client_port, message.get())) {
      ignore_result(message.release());
      break;
    }

    base::PlatformThread::YieldCurrentThread();
  }
}

void ChannelEndpoint::DetachFromChannel() {
  scoped_refptr<ChannelEndpointClient> client;
  unsigned client_port = 0;
  {
    base::AutoLock locker(lock_);

    if (client_) {
      // Take a ref, and call |OnDetachFromChannel()| outside the lock.
      client = client_;
      client_port = client_port_;
    }

    // |channel_| may already be null if we already detached from the channel in
    // |DetachFromClient()| by calling |Channel::DetachEndpoint()| (and there
    // are racing detaches).
    if (channel_)
      ResetChannelNoLock();
    else
      DCHECK(is_detached_from_channel_);
  }

  // If |ReplaceClient()| is called (from another thread) after the above locked
  // section but before we call |OnDetachFromChannel()|, |ReplaceClient()|
  // return false to notify the caller that the channel was already detached.
  // (The old client has to accept the arguably-spurious call to
  // |OnDetachFromChannel()|.)
  if (client)
    client->OnDetachFromChannel(client_port);
}

ChannelEndpoint::~ChannelEndpoint() {
  DCHECK(!client_);
  DCHECK(!channel_);
  DCHECK(!local_id_.is_valid());
  DCHECK(!remote_id_.is_valid());
}

bool ChannelEndpoint::WriteMessageNoLock(scoped_ptr<MessageInTransit> message) {
  DCHECK(message);

  lock_.AssertAcquired();

  DCHECK(channel_);
  DCHECK(local_id_.is_valid());
  DCHECK(remote_id_.is_valid());

  message->SerializeAndCloseDispatchers(channel_);
  message->set_source_id(local_id_);
  message->set_destination_id(remote_id_);
  return channel_->WriteMessage(message.Pass());
}

void ChannelEndpoint::ResetChannelNoLock() {
  DCHECK(channel_);
  DCHECK(local_id_.is_valid());
  DCHECK(remote_id_.is_valid());
  DCHECK(!is_detached_from_channel_);

  channel_ = nullptr;
  local_id_ = ChannelEndpointId();
  remote_id_ = ChannelEndpointId();
  is_detached_from_channel_ = true;
}

}  // namespace system
}  // namespace mojo
