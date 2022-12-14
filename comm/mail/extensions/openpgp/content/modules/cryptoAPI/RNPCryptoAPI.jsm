/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

"use strict";

var EXPORTED_SYMBOLS = ["getRNPAPI"];

const { RNP } = ChromeUtils.import("chrome://openpgp/content/modules/RNP.jsm");

Services.scriptloader.loadSubScript(
  "chrome://openpgp/content/modules/cryptoAPI/interface.js",
  null,
  "UTF-8"
); /* global CryptoAPI */

const { EnigmailLog } = ChromeUtils.import(
  "chrome://openpgp/content/modules/log.jsm"
);
const { EnigmailConstants } = ChromeUtils.import(
  "chrome://openpgp/content/modules/constants.jsm"
);
const { MailStringUtils } = ChromeUtils.import(
  "resource:///modules/MailStringUtils.jsm"
);

/**
 * RNP implementation of CryptoAPI
 */
class RNPCryptoAPI extends CryptoAPI {
  constructor() {
    super();
    this.api_name = "RNP";
  }

  /**
   * Get the list of all knwn keys (including their secret keys)
   * @param {Array of String} onlyKeys: [optional] only load data for specified key IDs
   *
   * @return {Promise<Array of Object>}
   */
  async getKeys(onlyKeys = null) {
    return RNP.getKeys(onlyKeys);
  }

  /**
   * Obtain signatures for a given set of key IDs.
   *
   * @param {String}  keyId:            space-separated list of key IDs
   * @param {Boolean} ignoreUnknownUid: if true, filter out unknown signer's UIDs
   *
   * @return {Promise<Array of Object>} - see extractSignatures()
   */
  async getKeySignatures(keyId, ignoreUnknownUid = false) {
    return RNP.getKeySignatures(keyId, ignoreUnknownUid);
  }

  /**
   * Obtain signatures for a given key.
   *
   * @param {KeyObj}  keyObj:           the signatures of this key will be returned
   * @param {Boolean} ignoreUnknownUid: if true, filter out unknown signer's UIDs
   *
   * @return {Promise<Array of Object>} - see extractSignatures()
   */
  async getKeyObjSignatures(keyId, ignoreUnknownUid = false) {
    return RNP.getKeyObjSignatures(keyId, ignoreUnknownUid);
  }

  /**
   * Export the minimum key for the public key object:
   * public key, primary user ID, newest encryption subkey
   *
   * @param {String} fpr:                a single FPR
   * @param {String} email:              [optional] the email address of the desired user ID.
   *                                     If the desired user ID cannot be found or is not valid, use the primary UID instead
   * @param {Array<Number>} subkeyDates: [optional] remove subkeys with specific creation Dates
   *
   * @return {Promise<Object>}:
   *    - exitCode (0 = success)
   *    - errorMsg (if exitCode != 0)
   *    - keyData: BASE64-encded string of key data
   */
  async getMinimalPubKey(fpr, email, subkeyDates) {
    throw new Error("Not implemented");
  }

  async importPubkeyBlockAutoAcceptAPI(
    win,
    keyBlock,
    acceptance,
    permissive,
    limitedFPRs = []
  ) {
    let res = await RNP.importPubkeyBlockAutoAcceptImpl(
      win,
      keyBlock,
      acceptance,
      permissive,
      limitedFPRs
    );
    return res;
  }

  /**
   * Import key(s) from a file
   *
   * @param {nsIFile} inputFile:  the file holding the keys
   *
   * @return {Object} or null in case no data / error:
   *   - {Number}          exitCode:        result code (0: OK)
   *   - {Array of String) importedKeys:    imported fingerprints
   *   - {String}          errorMsg:        human readable error message
   */
  async importSecKeyFromFileAPI(win, passCB, inputFile, permissive) {
    let contents = null;
    try {
      let data = await IOUtils.read(inputFile.path);
      contents = MailStringUtils.uint8ArrayToByteString(data);
    } catch (ex) {
      console.debug(ex);
    }
    if (!contents) {
      return null;
    }

    return RNP.importSecKeyBlockImpl(win, passCB, contents, permissive);
  }

  async importRevBlockAPI(data) {
    return RNP.importRevImpl(data);
  }

  /**
   * Export secret key(s) to a file
   *
   * @param {String}  keyId      Specification by fingerprint or keyID
   * @param {Boolean} minimalKey  if true, reduce key to minimum required
   *
   * @return {Object}:
   *   - {Number} exitCode:  result code (0: OK)
   *   - {String} keyData:   ASCII armored key data material
   *   - {String} errorMsg:  error message in case exitCode !== 0
   */

  async extractSecretKey(keyId, minimalKey) {
    throw new Error("extractSecretKey not implemented");
  }

  /**
   *
   * @param {byte} byteData    The encrypted data
   *
   * @return {String or null} - the name of the attached file
   */

  async getFileName(byteData) {
    throw new Error("getFileName not implemented");
  }

  /**
   *
   * @param {Path} filePath    The signed file
   * @param {Path} sigPath       The signature to verify
   *
   * @return {Promise<String>} - A message from the verification.
   *
   * Use Promise.catch to handle failed verifications.
   * The message will be an error message in this case.
   */

  async verifyAttachment(filePath, sigPath) {
    throw new Error("verifyAttachment not implemented");
  }

  /**
   *
   * @param {Bytes}  encrypted     The encrypted data
   *
   * @return {Promise<Object>} - Return object with decryptedData and
   * status information
   *
   * Use Promise.catch to handle failed decryption.
   * retObj.errorMsg will be an error message in this case.
   */

  async decryptAttachment(encrypted) {
    let options = {};
    options.fromAddr = "";
    options.msgDate = null;
    return RNP.decrypt(encrypted, options);
  }

  /**
   *
   * @param {String} encrypted     The encrypted data
   * @param {Object} options       Decryption options
   *
   * @return {Promise<Object>} - Return object with decryptedData and
   * status information
   *
   * Use Promise.catch to handle failed decryption.
   * retObj.errorMsg will be an error message in this case.
   *   XXX: it's not... ^^^ This should be changed to always reject
   *     by throwing an Error (subclass?) for failures to decrypt.
   */

  async decrypt(encrypted, options) {
    EnigmailLog.DEBUG(`rnp-cryptoAPI.js: decrypt()\n`);

    return RNP.decrypt(encrypted, options);
  }

  /**
   *
   * @param {String} encrypted     The encrypted data
   * @param {Object} options       Decryption options
   *
   * @return {Promise<Object>} - Return object with decryptedData and
   * status information
   *
   * Use Promise.catch to handle failed decryption.
   * retObj.errorMsg will be an error message in this case.
   */

  async decryptMime(encrypted, options) {
    EnigmailLog.DEBUG(`rnp-cryptoAPI.js: decryptMime()\n`);

    // write something to gpg such that the process doesn't get stuck
    if (encrypted.length === 0) {
      encrypted = "NO DATA\n";
    }

    options.noOutput = false;
    options.verifyOnly = false;
    options.uiFlags = EnigmailConstants.UI_PGP_MIME;

    return this.decrypt(encrypted, options);
  }

  /**
   *
   * @param {String} signed        The signed data
   * @param {Object} options       Decryption options
   *
   * @return {Promise<Object>} - Return object with decryptedData and
   * status information
   *
   * Use Promise.catch to handle failed decryption.
   * retObj.errorMsg will be an error message in this case.
   */

  async verifyMime(signed, options) {
    EnigmailLog.DEBUG(`rnp-cryptoAPI.js: verifyMime()\n`);

    //options.noOutput = true;
    //options.verifyOnly = true;
    //options.uiFlags = EnigmailConstants.UI_PGP_MIME;

    if (!options.mimeSignatureData) {
      throw new Error("inline verify not yet implemented");
    }
    return RNP.verifyDetached(signed, options);
  }

  async getKeyListFromKeyBlockAPI(
    keyBlockStr,
    pubkey,
    seckey,
    permissive,
    withPubKey
  ) {
    return RNP.getKeyListFromKeyBlockImpl(
      keyBlockStr,
      pubkey,
      seckey,
      permissive,
      withPubKey
    );
  }

  async genKey(userId, keyType, keySize, expiryTime, passphrase) {
    let id = RNP.genKey(userId, keyType, keySize, expiryTime, passphrase);
    await RNP.saveKeyRings();
    return id;
  }

  async deleteKey(keyFingerprint, deleteSecret) {
    return RNP.deleteKey(keyFingerprint, deleteSecret);
  }

  async encryptAndOrSign(plaintext, args, resultStatus) {
    return RNP.encryptAndOrSign(plaintext, args, resultStatus);
  }

  async getNewRevocation(id) {
    return RNP.getNewRevocation(id);
  }

  async getPublicKey(id) {
    return RNP.getPublicKey(id);
  }
}

function getRNPAPI() {
  return new RNPCryptoAPI();
}
