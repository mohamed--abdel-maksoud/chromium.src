// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function testGetService() {
  chrome.test.assertTrue(service != null);

  chrome.test.assertEq(serviceId, service.instanceId);
  chrome.test.assertEq('00001234-0000-1000-8000-00805f9b34fb', service.uuid);
  chrome.test.assertEq(true , service.isPrimary);
  chrome.test.assertEq(false, service.isLocal);
  chrome.test.assertEq(deviceAddress, service.deviceAddress);

  chrome.test.succeed();
}

var deviceAddress = '11:22:33:44:55:66';
var serviceId = 'service_id0';
var badServiceId = 'service_id1';

var service = null;

function failOnError() {
  if (chrome.runtime.lastError) {
    chrome.test.fail(chrome.runtime.lastError.message);
  }
}

// 1. Unknown service instanceId.
chrome.bluetoothLowEnergy.getService(badServiceId, function(result) {
  if (result || !chrome.runtime.lastError) {
    chrome.test.fail('Unexpected service.');
  }

  // 2. Known service instanceId, but the mapped device is unknown.
  chrome.bluetoothLowEnergy.getService(serviceId, function(result) {
    if (result || !chrome.runtime.lastError) {
      chrome.test.fail('Unexpected service.');
    }

    // 3. Known service instanceId, but the mapped device does not know about
    // the service.
    chrome.bluetoothLowEnergy.getService(serviceId, function(result) {
      if (result || !chrome.runtime.lastError) {
        chrome.test.fail('Unexpected service.');
      }

      // 4. Success.
      chrome.bluetoothLowEnergy.getService(serviceId, function(result) {
        failOnError();
        service = result;

        chrome.test.sendMessage('ready', function(message) {
          chrome.test.runTests([testGetService]);
        });
      });
    });
  });
});
