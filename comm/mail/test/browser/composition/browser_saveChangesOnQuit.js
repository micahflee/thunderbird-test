/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Tests that we prompt the user if they'd like to save their message when they
 * try to quit/close with an open compose window with unsaved changes, and
 * that we don't prompt if there are no changes.
 */

"use strict";

var {
  close_compose_window,
  open_compose_new_mail,
  open_compose_with_forward,
  open_compose_with_reply,
} = ChromeUtils.import("resource://testing-common/mozmill/ComposeHelpers.jsm");
var {
  add_message_to_folder,
  assert_selected_and_displayed,
  be_in_folder,
  create_folder,
  create_message,
  mc,
  select_click_row,
} = ChromeUtils.import(
  "resource://testing-common/mozmill/FolderDisplayHelpers.jsm"
);
var { gMockPromptService } = ChromeUtils.import(
  "resource://testing-common/mozmill/PromptHelpers.jsm"
);

var SAVE = 0;
var CANCEL = 1;
var DONT_SAVE = 2;

var cwc = null; // compose window controller
var folder = null;

add_setup(async function() {
  requestLongerTimeout(3);

  folder = await create_folder("PromptToSaveTest");

  await add_message_to_folder([folder], create_message()); // row 0
  let localFolder = folder.QueryInterface(Ci.nsIMsgLocalMailFolder);
  localFolder.addMessage(msgSource("content type: text", "text")); // row 1
  localFolder.addMessage(msgSource("content type missing", null)); // row 2
});

function msgSource(aSubject, aContentType) {
  let msgId = Services.uuid.generateUUID() + "@invalid";

  return (
    "From - Sun Apr 07 22:47:11 2013\r\n" +
    "X-Mozilla-Status: 0001\r\n" +
    "X-Mozilla-Status2: 00000000\r\n" +
    "Message-ID: <" +
    msgId +
    ">\r\n" +
    "Date: Sun, 07 Apr 2013 22:47:11 +0300\r\n" +
    "From: Someone <some.one@invalid>\r\n" +
    "To: someone.else@invalid\r\n" +
    "Subject: " +
    aSubject +
    "\r\n" +
    "MIME-Version: 1.0\r\n" +
    (aContentType ? "Content-Type: " + aContentType + "\r\n" : "") +
    "Content-Transfer-Encoding: 7bit\r\n\r\n" +
    "A msg with contentType " +
    aContentType +
    "\r\n"
  );
}

/**
 * Test that when a compose window is open with changes, and
 * a Quit is requested (for example, from File > Quit from the
 * 3pane), that the user gets a confirmation dialog to discard
 * the changes. This also tests that the user can cancel the
 * quit request.
 */
add_task(function test_can_cancel_quit_on_changes() {
  // Register the Mock Prompt Service
  gMockPromptService.register();

  // opening a new compose window
  cwc = open_compose_new_mail(mc);

  // Make some changes
  cwc.type(cwc.e("messageEditor"), "Hey check out this megalol link");

  let cancelQuit = Cc["@mozilla.org/supports-PRBool;1"].createInstance(
    Ci.nsISupportsPRBool
  );

  // Set the Mock Prompt Service to return false, so that we
  // cancel the quit.
  gMockPromptService.returnValue = CANCEL;
  // Trigger the quit-application-request notification

  Services.obs.notifyObservers(cancelQuit, "quit-application-requested");

  let promptState = gMockPromptService.promptState;
  Assert.notEqual(null, promptState, "Expected a confirmEx prompt");

  Assert.equal("confirmEx", promptState.method);
  // Since we returned false on the confirmation dialog,
  // we should be cancelling the quit - so cancelQuit.data
  // should now be true
  Assert.ok(cancelQuit.data, "Didn't cancel the quit");

  close_compose_window(cwc);

  // Unregister the Mock Prompt Service
  gMockPromptService.unregister();
});

/**
 * Test that when a compose window is open with changes, and
 * a Quit is requested (for example, from File > Quit from the
 * 3pane), that the user gets a confirmation dialog to discard
 * the changes. This also tests that the user can let the quit
 * occur.
 */
add_task(function test_can_quit_on_changes() {
  // Register the Mock Prompt Service
  gMockPromptService.register();

  // opening a new compose window
  cwc = open_compose_new_mail(mc);

  // Make some changes
  cwc.type(cwc.e("messageEditor"), "Hey check out this megalol link");

  let cancelQuit = Cc["@mozilla.org/supports-PRBool;1"].createInstance(
    Ci.nsISupportsPRBool
  );

  // Set the Mock Prompt Service to return true, so that we're
  // allowing the quit to occur.
  gMockPromptService.returnValue = DONT_SAVE;

  // Trigger the quit-application-request notification
  Services.obs.notifyObservers(cancelQuit, "quit-application-requested");

  let promptState = gMockPromptService.promptState;
  Assert.notEqual(null, promptState, "Expected a confirmEx prompt");

  Assert.equal("confirmEx", promptState.method);
  // Since we returned true on the confirmation dialog,
  // we should be quitting - so cancelQuit.data should now be
  // false
  Assert.ok(!cancelQuit.data, "The quit request was cancelled");

  close_compose_window(cwc);

  // Unregister the Mock Prompt Service
  gMockPromptService.unregister();
});

/**
 * Bug 698077 - test that when quitting with two compose windows open, if
 * one chooses "Don't Save", and the other chooses "Cancel", that the first
 * window's state is such that subsequent quit requests still cause the
 * Don't Save / Cancel / Save dialog to come up.
 */
add_task(function test_window_quit_state_reset_on_aborted_quit() {
  // Register the Mock Prompt Service
  gMockPromptService.register();

  // open two new compose windows
  let cwc1 = open_compose_new_mail(mc);
  let cwc2 = open_compose_new_mail(mc);

  // Type something in each window.
  cwc1.type(cwc1.e("messageEditor"), "Marco!");
  cwc2.type(cwc2.e("messageEditor"), "Polo!");

  let cancelQuit = Cc["@mozilla.org/supports-PRBool;1"].createInstance(
    Ci.nsISupportsPRBool
  );

  // This is a hacky method for making sure that the second window
  // receives a CANCEL click in the popup dialog.
  var numOfPrompts = 0;
  gMockPromptService.onPromptCallback = function() {
    numOfPrompts++;

    if (numOfPrompts > 1) {
      gMockPromptService.returnValue = CANCEL;
    }
  };

  gMockPromptService.returnValue = DONT_SAVE;

  // Trigger the quit-application-request notification
  Services.obs.notifyObservers(cancelQuit, "quit-application-requested");

  // We should have cancelled the quit appropriately.
  Assert.ok(cancelQuit.data);

  // The quit behaviour is that the second window to spawn is the first
  // one that prompts for Save / Don't Save, etc.
  gMockPromptService.reset();

  // The first window should still prompt when attempting to close the
  // window.
  gMockPromptService.returnValue = DONT_SAVE;
  cwc2.click(cwc2.e("menu_close"));

  let promptState = gMockPromptService.promptState;
  Assert.notEqual(null, promptState, "Expected a confirmEx prompt");

  close_compose_window(cwc1);

  gMockPromptService.unregister();
});

/**
 * Tests that we don't get a prompt to save if there has been no user input
 * into the message yet, when trying to close.
 */
add_task(function test_no_prompt_on_close_for_unmodified() {
  be_in_folder(folder);
  let msg = select_click_row(0);
  assert_selected_and_displayed(mc, msg);

  let nwc = open_compose_new_mail();
  close_compose_window(nwc, false);

  let rwc = open_compose_with_reply();
  close_compose_window(rwc, false);

  let fwc = open_compose_with_forward();
  close_compose_window(fwc, false);
});

/**
 * Tests that we get a prompt to save if the user made changes to the message
 * before trying to close it.
 */
add_task(function test_prompt_on_close_for_modified() {
  be_in_folder(folder);
  let msg = select_click_row(0);
  assert_selected_and_displayed(mc, msg);

  let nwc = open_compose_new_mail();
  nwc.type(nwc.e("messageEditor"), "Hey hey hey!");
  close_compose_window(nwc, true);

  let rwc = open_compose_with_reply();
  rwc.type(rwc.e("messageEditor"), "Howdy!");
  close_compose_window(rwc, true);

  let fwc = open_compose_with_forward();
  fwc.type(fwc.e("messageEditor"), "Greetings!");
  close_compose_window(fwc, true);
});

/**
 * Test there's no prompt on close when no changes was made in reply/forward
 * windows - for the case the original msg had content type "text".
 */
add_task(function test_no_prompt_on_close_for_unmodified_content_type_text() {
  be_in_folder(folder);
  let msg = select_click_row(1); // row 1 is the one with content type text
  assert_selected_and_displayed(mc, msg);

  let rwc = open_compose_with_reply();
  close_compose_window(rwc, false);

  let fwc = open_compose_with_forward();
  Assert.equal(
    fwc.e("attachmentBucket").getRowCount(),
    0,
    "forwarding msg created attachment"
  );
  close_compose_window(fwc, false);
});

/**
 * Test there's no prompt on close when no changes was made in reply/forward
 * windows - for the case the original msg had no content type.
 */
add_task(function test_no_prompt_on_close_for_unmodified_no_content_type() {
  be_in_folder(folder);
  let msg = select_click_row(2); // row 2 is the one with no content type
  assert_selected_and_displayed(mc, msg);

  let rwc = open_compose_with_reply();
  close_compose_window(rwc, false);

  let fwc = open_compose_with_forward();
  Assert.equal(
    fwc.e("attachmentBucket").getRowCount(),
    0,
    "forwarding msg created attachment"
  );
  close_compose_window(fwc, false);
});
