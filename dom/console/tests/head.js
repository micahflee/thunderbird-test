/**
 * Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

const ConsoleAPIStorage = SpecialPowers.Cc[
  "@mozilla.org/consoleAPI-storage;1"
].getService(SpecialPowers.Ci.nsIConsoleAPIStorage);

// This is intended to just be a drop-in replacement for an old observer
// notification.
function addConsoleStorageListener(listener) {
  listener.__handler = (message, id) => {
    listener.observe(message, id);
  };
  ConsoleAPIStorage.addLogEventListener(
    listener.__handler,
    SpecialPowers.wrap(document).nodePrincipal
  );
}

function removeConsoleStorageListener(listener) {
  ConsoleAPIStorage.removeLogEventListener(listener.__handler);
}
