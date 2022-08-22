/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

var { wait_for_element_visible } = ChromeUtils.import(
  "resource://testing-common/mozmill/DOMHelpers.jsm"
);
var { open_message_from_file } = ChromeUtils.import(
  "resource://testing-common/mozmill/FolderDisplayHelpers.jsm"
);
var { close_window } = ChromeUtils.import(
  "resource://testing-common/mozmill/WindowHelpers.jsm"
);

/**
 * Bug 1358565
 * Check that a non-empty image is shown as attachment and is detected as non-empty
 * when message is viewed as plain text.
 */
add_task(async function test_attachment_not_empty() {
  Services.prefs.setBoolPref("mailnews.display.prefer_plaintext", true);

  let file = new FileUtils.File(getTestFilePath("data/bug1358565.eml"));

  let msgc = await open_message_from_file(file);

  wait_for_element_visible(msgc, "attachmentToggle");
  msgc.click(msgc.e("attachmentToggle"));

  wait_for_element_visible(msgc, "attachmentList");
  Assert.equal(msgc.e("attachmentList").itemCount, 1);

  let attachmentElem = msgc.e("attachmentList").getItemAtIndex(0);
  Assert.equal(attachmentElem.attachment.contentType, "image/jpeg");
  Assert.equal(attachmentElem.attachment.name, "bug.png");
  Assert.ok(attachmentElem.attachment.hasFile);
  Assert.ok(
    !(await attachmentElem.attachment.isEmpty()),
    "Attachment incorrectly determined empty"
  );

  close_window(msgc);

  Services.prefs.clearUserPref("mailnews.display.prefer_plaintext");
});
