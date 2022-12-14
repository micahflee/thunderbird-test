/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Test that the folder pane collapses properly, stays collapsed amongst tab
 * changes, and that persistence works (to a first approximation).
 */

"use strict";

var {
  be_in_folder,
  close_tab,
  create_folder,
  make_message_sets_in_folders,
  mc,
  open_folder_in_new_tab,
  open_selected_message_in_new_tab,
  select_click_row,
  switch_tab,
} = ChromeUtils.import(
  "resource://testing-common/mozmill/FolderDisplayHelpers.jsm"
);

var folder;

add_setup(async function() {
  folder = await create_folder("FolderPaneVisibility");
  await make_message_sets_in_folders([folder], [{ count: 3 }]);
});

/**
 * When displaying a folder, assert that the folder pane is visible and all the
 * menus, splitters, etc. are set up right.
 */
function assert_folder_pane_visible() {
  if (!mc.folderDisplay.folderPaneVisible) {
    throw new Error(
      "The folder display does not think that the folder pane " +
        "is visible, but it should!"
    );
  }

  // - folder pane should be visible
  if (mc.e("folderPaneBox").collapsed === true) {
    throw new Error("folderPaneBox should not be collapsed!");
  }

  // - the folder pane splitter should not be collapsed
  if (mc.e("folderpane_splitter").collapsed === true) {
    throw new Error("folderpane_splitter should not be collapsed!");
  }

  // - the menu item should be checked
  // force the view menu to update.
  mc.window.view_init();
  let paneMenuItem = mc.e("menu_showFolderPane");
  if (paneMenuItem.getAttribute("checked") != "true") {
    throw new Error("The Folder Pane menu item should be checked.");
  }
  Assert.ok(true, "assert_folder_pane_visible ran to completion");
}

/**
 * When displaying a folder, assert that the folder pane is hidden and all the
 * menus, splitters, etc. are set up right.
 *
 * @param aFolderPaneIllegal Is the folder pane illegal to display at this time?
 *     This impacts whether the folder pane splitter should be visible.
 */
function assert_folder_pane_hidden(aFolderPaneIllegal) {
  if (mc.folderDisplay.folderPaneVisible) {
    throw new Error(
      "The folder display thinks that the folder pane is " +
        "visible, but it shouldn't!"
    );
  }

  // - folder pane shouldn't be visible
  if (mc.e("folderPaneBox").collapsed === false) {
    throw new Error("folderPaneBox should be collapsed!");
  }

  // force the view menu to update.
  mc.window.view_init();
  let paneMenuItem = mc.e("menu_showFolderPane");
  // - the folder pane splitter should or should not be collapsed, depending on
  //   aFolderPaneIllegal
  if (aFolderPaneIllegal) {
    if (mc.e("folderpane_splitter").collapsed === false) {
      throw new Error("folderpane_splitter should be collapsed!");
    }
    // if (paneMenuItem.getAttribute("disabled") != "true")
    //  throw new Error("The Folder Pane menu item should be disabled.");
  } else {
    if (mc.e("folderpane_splitter").collapsed === true) {
      throw new Error("folderpane_splitter should not be collapsed!");
    }
    if (paneMenuItem.getAttribute("checked") == "true") {
      throw new Error("The Folder Pane menu item should not be checked.");
    }
  }
  Assert.ok(true, "assert_folder_pane_hidden ran to completion");
}

function toggle_folder_pane() {
  // Since we don't have a shortcut to toggle the folder pane, we're going to
  // have to collapse it ourselves
  mc.window.MsgToggleFolderPane();
  Assert.ok(true, "toggle_folder_pane ran to completion");
}

/**
 * By default, the folder pane should be visible.
 */
add_task(function test_folder_pane_visible_state_is_right() {
  be_in_folder(folder);
  assert_folder_pane_visible();
  Assert.ok(true, "test_folder_pane_visible_state_is_right ran to completion");
});

/**
 * Toggle the folder pane off.
 */
add_task(function test_toggle_folder_pane_off() {
  toggle_folder_pane();
  assert_folder_pane_hidden();
  Assert.ok(true, "test_toggle_folder_pane_off ran to completion");
});

/**
 * Toggle the folder pane on.
 */
add_task(function test_toggle_folder_pane_on() {
  toggle_folder_pane();
  assert_folder_pane_visible();
  Assert.ok(true, "test_toggle_folder_pane_on ran to completion");
});

/**
 * Make sure that switching to message tabs of folder tabs with a different
 * folder pane state does not break. This test should cover all transition
 * states.
 */
add_task(function test_folder_pane_is_sticky() {
  let tabFolderA = be_in_folder(folder);
  assert_folder_pane_visible();

  // [folder+ => (new) message]
  select_click_row(0);
  let tabMessage = open_selected_message_in_new_tab();
  assert_folder_pane_hidden(true);

  // [message => folder+]
  switch_tab(tabFolderA);
  assert_folder_pane_visible();

  // [folder+ => (new) folder+]
  let tabFolderB = open_folder_in_new_tab(folder);
  assert_folder_pane_visible();

  // [folder pane toggle + => -]
  toggle_folder_pane();
  assert_folder_pane_hidden();

  // [folder- => folder+]
  switch_tab(tabFolderA);
  assert_folder_pane_visible();

  // (redundant) [ folder pane toggle + => -]
  toggle_folder_pane();
  assert_folder_pane_hidden();

  // [folder- => message]
  switch_tab(tabMessage);
  assert_folder_pane_hidden(true);

  // [message => folder-]
  close_tab(tabMessage);
  assert_folder_pane_hidden();

  // [folder- => (new) folder-]
  // (we are testing inheritance here)
  let tabFolderC = open_folder_in_new_tab(folder);
  assert_folder_pane_hidden();

  // [folder- => folder-]
  close_tab(tabFolderC);
  // the tab we are on now doesn't matter, so we don't care
  assert_folder_pane_hidden();
  switch_tab(tabFolderB);

  // [ folder pane toggle - => + ]
  toggle_folder_pane();
  assert_folder_pane_visible();

  // [folder+ => folder-]
  close_tab(tabFolderB);
  assert_folder_pane_hidden();

  // (redundant) [ folder pane toggle - => + ]
  toggle_folder_pane();
  assert_folder_pane_visible();

  Assert.ok(true, "test_folder_pane_is_sticky ran to completion");
});

/**
 * Test that if we serialize and restore the tabs then the folder pane is in the
 * expected collapsed/non-collapsed state. Because of the special "first tab"
 * situation, we need to do this twice to test each case for the first tab.  For
 * additional thoroughness we also flip the state we have the other tabs be in.
 */
add_task(function test_folder_pane_persistence_generally_works() {
  be_in_folder(folder);

  // helper to open tabs with the folder pane in the desired states (1 for
  //  visible, 0 for hidden)
  function openTabs(aConfig) {
    let curState;
    for (let [iTab, folderPaneVisible] of aConfig.entries()) {
      if (iTab == 0) {
        curState = folderPaneVisible;
      } else {
        open_folder_in_new_tab(folder);
        if (curState != folderPaneVisible) {
          toggle_folder_pane();
          curState = folderPaneVisible;
        }
      }
    }
  }

  // close everything but the first tab.
  function closeTabs() {
    while (mc.tabmail.tabInfo.length > 1) {
      mc.tabmail.closeTab(1);
    }
  }

  function verifyTabs(aConfig) {
    for (let [iTab, folderPaneVisible] of aConfig.entries()) {
      switch_tab(iTab);
      dump(" checking tab: " + iTab + "\n");
      if (folderPaneVisible) {
        assert_folder_pane_visible();
      } else {
        assert_folder_pane_hidden();
      }
    }
  }

  let configs = [
    // 1st time: [+ - - + +]
    [1, 0, 0, 1, 1],
    // 2nd time: [- + + - -]
    [0, 1, 1, 0, 0],
  ];

  for (let config of configs) {
    openTabs(config);
    verifyTabs(config); // make sure openTabs did its job right
    let state = mc.tabmail.persistTabs();
    closeTabs();
    // toggle the state for the current tab so we can be sure that it knows how
    // to change things.
    toggle_folder_pane();
    SimpleTest.ignoreAllUncaughtExceptions(true);
    mc.tabmail.restoreTabs(state);
    verifyTabs(config);
    SimpleTest.ignoreAllUncaughtExceptions(false);
    closeTabs();

    // toggle the first tab again.  This sets - properly for the second pass and
    // restores it to + for when we are done.
    toggle_folder_pane();
  }
  // For one last time, make sure.
  assert_folder_pane_visible();

  Assert.ok(
    true,
    "test_folder_pane_persistence_generally_works ran to completion"
  );
});
