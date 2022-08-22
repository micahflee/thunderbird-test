/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

const TEST_URI =
  "data:text/html,<!DOCTYPE html>Test <code>help()</code> jsterm helper";
const HELP_URL =
  "https://firefox-source-docs.mozilla.org/devtools-user/web_console/helpers/";

add_task(async function() {
  const hud = await openNewTabAndConsole(TEST_URI);

  let openedLinks = 0;
  const oldOpenLink = hud.openLink;
  hud.openLink = url => {
    if (url == HELP_URL) {
      openedLinks++;
    }
  };

  await clearOutput(hud);
  execute(hud, "help()");
  execute(hud, "help");
  execute(hud, "?");
  // Wait for a simple message to be displayed so we know the different help commands
  // were processed.
  await executeAndWaitForResultMessage(hud, "smoke", "");

  const messages = hud.ui.outputNode.querySelectorAll(".message");
  is(messages.length, 5, "There is the expected number of messages");
  const resultMessages = hud.ui.outputNode.querySelectorAll(".result");
  is(
    resultMessages.length,
    1,
    "There is no results shown for the help commands"
  );

  is(openedLinks, 3, "correct number of pages opened by the help calls");
  hud.openLink = oldOpenLink;
});
