/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Test that the smart folder mode works properly. This includes checking
 * whether |getParentOfFolder| works, and also making sure selectFolder behaves
 * properly, opening the right folders.
 */

"use strict";

var {
  archive_selected_messages,
  assert_folder_child_in_view,
  assert_folder_collapsed,
  assert_folder_expanded,
  assert_folder_selected_and_displayed,
  be_in_folder,
  collapse_folder,
  delete_messages,
  expand_folder,
  FAKE_SERVER_HOSTNAME,
  get_smart_folder_named,
  get_special_folder,
  inboxFolder,
  make_message_sets_in_folders,
  mc,
  select_click_row,
} = ChromeUtils.import(
  "resource://testing-common/mozmill/FolderDisplayHelpers.jsm"
);
var { MailServices } = ChromeUtils.import(
  "resource:///modules/MailServices.jsm"
);

var rootFolder;
var inboxSubfolder;
var trashFolder;
var trashSubfolder;

var smartInboxFolder;

var inboxSet;

add_setup(async function() {
  rootFolder = inboxFolder.server.rootFolder;
  // Create a folder as a subfolder of the inbox
  inboxFolder.createSubfolder("SmartFoldersA", null);
  inboxSubfolder = inboxFolder.getChildNamed("SmartFoldersA");

  trashFolder = inboxFolder.server.rootFolder.getFolderWithFlags(
    Ci.nsMsgFolderFlags.Trash
  );
  trashFolder.createSubfolder("SmartFoldersB", null);
  trashSubfolder = trashFolder.getChildNamed("SmartFoldersB");

  // The message itself doesn't really matter, as long as there's at least one
  // in the folder.
  [inboxSet] = await make_message_sets_in_folders(
    [inboxFolder],
    [{ count: 1 }]
  );
  await make_message_sets_in_folders([inboxSubfolder], [{ count: 1 }]);
});

/**
 * Assert that the given folder is considered to be the container of the given
 * message header in this folder mode.
 */
function assert_folder_for_msg_hdr(aMsgHdr, aFolder) {
  let actualFolder = mc.folderTreeView.getFolderForMsgHdr(aMsgHdr);
  if (actualFolder != aFolder) {
    throw new Error(
      "Message " +
        aMsgHdr.messageId +
        " should be contained in folder " +
        aFolder.URI +
        "in this view, but is actually contained in " +
        actualFolder.URI
    );
  }
}

/**
 * Switch to the smart folder mode.
 */
add_task(function test_switch_to_smart_folders() {
  mc.folderTreeView.activeModes = "smart";
  // Hide the all folders view.
  mc.folderTreeView.activeModes = "all";

  // The smart inbox may not have been created at setup time, so get it now.
  smartInboxFolder = get_smart_folder_named("Inbox");
});

/**
 * Test the getParentOfFolder function.
 */
add_task(function test_get_parent_of_folder() {
  // An inbox should have the special inbox as its parent
  assert_folder_child_in_view(inboxFolder, smartInboxFolder);
  // Similarly for the trash folder
  assert_folder_child_in_view(trashFolder, get_smart_folder_named("Trash"));

  // A child of the inbox (a shallow special folder) should have the account's
  // root folder as its parent
  assert_folder_child_in_view(inboxSubfolder, rootFolder);
  // A child of the trash (a deep special folder) should have the trash itself
  // as its parent
  assert_folder_child_in_view(trashSubfolder, trashFolder);

  // Subfolders of subfolders of the inbox should behave as normal
  inboxSubfolder.createSubfolder("SmartFoldersC", null);
  assert_folder_child_in_view(
    inboxSubfolder.getChildNamed("SmartFoldersC"),
    inboxSubfolder
  );
});

/**
 * Test the getFolderForMsgHdr function.
 */
add_task(function test_get_folder_for_msg_hdr() {
  be_in_folder(inboxFolder);
  let inboxMsgHdr = mc.dbView.getMsgHdrAt(0);
  assert_folder_for_msg_hdr(inboxMsgHdr, smartInboxFolder);

  be_in_folder(inboxSubfolder);
  let inboxSubMsgHdr = mc.dbView.getMsgHdrAt(0);
  assert_folder_for_msg_hdr(inboxSubMsgHdr, inboxSubfolder);
});

/**
 * Test that selectFolder expands a collapsed smart inbox.
 */
add_task(function test_select_folder_expands_collapsed_smart_inbox() {
  // Collapse the smart inbox
  collapse_folder(smartInboxFolder);
  assert_folder_collapsed(smartInboxFolder);

  // Also collapse the account root, make sure selectFolder don't expand it
  collapse_folder(rootFolder);
  assert_folder_collapsed(rootFolder);

  // Now attempt to select the folder.
  mc.folderTreeView.selectFolder(inboxFolder);

  assert_folder_collapsed(rootFolder);
  assert_folder_expanded(smartInboxFolder);
  assert_folder_selected_and_displayed(inboxFolder);
});

/**
 * Test that selectFolder expands a collapsed account root.
 */
add_task(function test_select_folder_expands_collapsed_account_root() {
  // Collapse the account root
  collapse_folder(rootFolder);
  assert_folder_collapsed(rootFolder);

  // Also collapse the smart inbox, make sure selectFolder don't expand it
  collapse_folder(smartInboxFolder);
  assert_folder_collapsed(smartInboxFolder);

  // Now attempt to select the folder.
  mc.folderTreeView.selectFolder(inboxSubfolder);

  assert_folder_collapsed(smartInboxFolder);
  assert_folder_expanded(rootFolder);
  assert_folder_selected_and_displayed(inboxSubfolder);
});

/**
 * Test that smart folders are updated when the folders they should be
 * searching over are added/removed or have the relevant flag set/cleared.
 */
add_task(async function test_folder_flag_changes() {
  expand_folder(smartInboxFolder);
  // Now attempt to select the folder.
  mc.folderTreeView.selectFolder(inboxSubfolder);
  // Need to archive two messages in two different accounts in order to
  // create a smart Archives folder.
  select_click_row(0);
  archive_selected_messages();
  let pop3Server = MailServices.accounts.findServer(
    "tinderbox",
    FAKE_SERVER_HOSTNAME,
    "pop3"
  );
  let pop3Inbox = await get_special_folder(
    Ci.nsMsgFolderFlags.Inbox,
    false,
    pop3Server
  );
  await make_message_sets_in_folders([pop3Inbox], [{ count: 1 }]);
  mc.folderTreeView.selectFolder(pop3Inbox);
  select_click_row(0);
  archive_selected_messages();

  let smartArchiveFolder = get_smart_folder_named("Archives");
  let archiveScope =
    "|" +
    smartArchiveFolder.msgDatabase.dBFolderInfo.getCharProperty(
      "searchFolderUri"
    ) +
    "|";
  // We should have both this account, and a folder corresponding
  // to this year in the scope.
  rootFolder = inboxFolder.server.rootFolder;
  let archiveFolder = rootFolder.getChildNamed("Archives");
  assert_folder_and_children_in_scope(archiveFolder, archiveScope);
  archiveFolder = pop3Server.rootFolder.getChildNamed("Archives");
  assert_folder_and_children_in_scope(archiveFolder, archiveScope);

  // Remove the archive flag, and make sure the archive folder and
  // its children are no longer in the search scope.
  archiveFolder.clearFlag(Ci.nsMsgFolderFlags.Archive);

  // Refresh the archive scope because clearing the flag should have
  // changed it.
  archiveScope =
    "|" +
    smartArchiveFolder.msgDatabase.dBFolderInfo.getCharProperty(
      "searchFolderUri"
    ) +
    "|";

  // figure out what we expect the archiveScope to now be.
  rootFolder = inboxFolder.server.rootFolder;
  let localArchiveFolder = rootFolder.getChildNamed("Archives");
  let desiredScope = "|" + localArchiveFolder.URI + "|";
  for (let folder of localArchiveFolder.descendants) {
    desiredScope += folder.URI + "|";
  }

  if (archiveScope != desiredScope) {
    throw new Error("archive scope wrong after removing folder");
  }
  assert_folder_and_children_not_in_scope(archiveFolder, archiveScope);
});

function assert_folder_and_children_in_scope(folder, searchScope) {
  let folderURI = "|" + folder.URI + "|";
  assert_uri_found(folderURI, searchScope);
  for (let f of folder.descendants) {
    assert_uri_found(f.URI, searchScope);
  }
}

function assert_folder_and_children_not_in_scope(folder, searchScope) {
  let folderURI = "|" + folder.URI + "|";
  assert_uri_not_found(folderURI, searchScope);
  for (let f of folder.descendants) {
    assert_uri_not_found(f.URI, searchScope);
  }
}

function assert_uri_found(folderURI, scopeList) {
  if (!scopeList.includes(folderURI)) {
    throw new Error("scope " + scopeList + "doesn't contain " + folderURI);
  }
}

function assert_uri_not_found(folderURI, scopeList) {
  if (scopeList.includes(folderURI)) {
    throw new Error(
      "scope " + scopeList + "contains " + folderURI + " but shouldn't"
    );
  }
}

/**
 * Move back to the all folders mode.
 */
add_task(function test_switch_to_all_folders() {
  mc.folderTreeView.activeModes = "smart";
});

registerCleanupFunction(async function() {
  inboxFolder.propagateDelete(inboxSubfolder, true, null);
  await delete_messages(inboxSet);
  trashFolder.propagateDelete(trashSubfolder, true, null);

  Assert.report(
    false,
    undefined,
    undefined,
    "Test ran to completion successfully"
  );
});
