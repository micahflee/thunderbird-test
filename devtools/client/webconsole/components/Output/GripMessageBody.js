/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

// React
const PropTypes = require("devtools/client/shared/vendor/react-prop-types");
const {
  MESSAGE_TYPE,
  JSTERM_COMMANDS,
} = require("devtools/client/webconsole/constants");
const {
  cleanupStyle,
} = require("devtools/client/shared/components/reps/reps/rep-utils");
const {
  getObjectInspector,
} = require("devtools/client/webconsole/utils/object-inspector");
const actions = require("devtools/client/webconsole/actions/index");

loader.lazyGetter(this, "objectInspector", function() {
  return require("devtools/client/shared/components/reps/index")
    .objectInspector;
});

loader.lazyGetter(this, "MODE", function() {
  return require("devtools/client/shared/components/reps/index").MODE;
});

GripMessageBody.displayName = "GripMessageBody";

GripMessageBody.propTypes = {
  grip: PropTypes.oneOfType([
    PropTypes.string,
    PropTypes.number,
    PropTypes.object,
  ]).isRequired,
  serviceContainer: PropTypes.shape({
    createElement: PropTypes.func.isRequired,
    onViewSourceInDebugger: PropTypes.func.isRequired,
  }),
  userProvidedStyle: PropTypes.string,
  useQuotes: PropTypes.bool,
  escapeWhitespace: PropTypes.bool,
  type: PropTypes.string,
  helperType: PropTypes.string,
  maybeScrollToBottom: PropTypes.func,
  setExpanded: PropTypes.func,
};

GripMessageBody.defaultProps = {
  mode: MODE.LONG,
};

function GripMessageBody(props) {
  const {
    grip,
    userProvidedStyle,
    serviceContainer,
    useQuotes,
    escapeWhitespace,
    mode = MODE.LONG,
    dispatch,
    maybeScrollToBottom,
    setExpanded,
    customFormat = false,
  } = props;

  let styleObject;
  if (userProvidedStyle && userProvidedStyle !== "") {
    styleObject = cleanupStyle(
      userProvidedStyle,
      serviceContainer.createElement
    );
  }

  const objectInspectorProps = {
    autoExpandDepth: shouldAutoExpandObjectInspector(props) ? 1 : 0,
    mode,
    maybeScrollToBottom,
    setExpanded,
    customFormat,
    onCmdCtrlClick: (node, { depth, event, focused, expanded }) => {
      const front = objectInspector.utils.node.getFront(node);
      if (front) {
        dispatch(actions.showObjectInSidebar(front));
      }
    },
  };

  if (
    typeof grip === "string" ||
    (grip && grip.type === "longString") ||
    (grip?.getGrip && grip.getGrip().type === "longString")
  ) {
    Object.assign(objectInspectorProps, {
      useQuotes,
      transformEmptyString: true,
      escapeWhitespace,
      style: styleObject,
    });
  }

  return getObjectInspector(grip, serviceContainer, objectInspectorProps);
}

function shouldAutoExpandObjectInspector(props) {
  const { helperType, type } = props;

  return type === MESSAGE_TYPE.DIR || helperType === JSTERM_COMMANDS.INSPECT;
}

module.exports = GripMessageBody;
