/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const EXPORTED_SYMBOLS = ["ircProtocol"];

var { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);
var { l10nHelper } = ChromeUtils.import("resource:///modules/imXPCOMUtils.jsm");
var { GenericProtocolPrototype } = ChromeUtils.import(
  "resource:///modules/jsProtoHelper.jsm"
);

const lazy = {};

XPCOMUtils.defineLazyGetter(lazy, "_", () =>
  l10nHelper("chrome://chat/locale/irc.properties")
);
ChromeUtils.defineModuleGetter(
  lazy,
  "ircAccount",
  "resource:///modules/ircAccount.jsm"
);

function ircProtocol() {
  // ircCommands.jsm exports one variable: commands. Import this directly into
  // the protocol object.
  this.commands = ChromeUtils.import(
    "resource:///modules/ircCommands.jsm"
  ).commands;
  this.registerCommands();
}
ircProtocol.prototype = {
  __proto__: GenericProtocolPrototype,
  get name() {
    return "IRC";
  },
  get normalizedName() {
    return "irc";
  },
  get iconBaseURI() {
    return "chrome://prpl-irc/skin/";
  },
  get usernameEmptyText() {
    return lazy._("irc.usernameHint");
  },

  usernameSplits: [
    {
      get label() {
        return lazy._("options.server");
      },
      separator: "@",
      defaultValue: "irc.libera.chat",
    },
  ],

  splitUsername(aName) {
    let splitter = aName.lastIndexOf("@");
    if (splitter === -1) {
      return [];
    }
    return [aName.slice(0, splitter), aName.slice(splitter + 1)];
  },

  options: {
    port: {
      get label() {
        return lazy._("options.port");
      },
      default: 6697,
    },
    ssl: {
      get label() {
        return lazy._("options.ssl");
      },
      default: true,
    },
    // TODO We should attempt to auto-detect encoding instead.
    encoding: {
      get label() {
        return lazy._("options.encoding");
      },
      default: "UTF-8",
    },
    quitmsg: {
      get label() {
        return lazy._("options.quitMessage");
      },
      get default() {
        return Services.prefs.getCharPref("chat.irc.defaultQuitMessage");
      },
    },
    partmsg: {
      get label() {
        return lazy._("options.partMessage");
      },
      default: "",
    },
    showServerTab: {
      get label() {
        return lazy._("options.showServerTab");
      },
      default: false,
    },
    alternateNicks: {
      get label() {
        return lazy._("options.alternateNicks");
      },
      default: "",
    },
  },

  get chatHasTopic() {
    return true;
  },
  get slashCommandsNative() {
    return true;
  },
  //  Passwords in IRC are optional, and are needed for certain functionality.
  get passwordOptional() {
    return true;
  },

  getAccount(aImAccount) {
    return new lazy.ircAccount(this, aImAccount);
  },
};
