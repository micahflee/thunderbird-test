/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 sw=2 et tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HTMLEditor.h"

#include <algorithm>
#include <utility>

#include "AutoRangeArray.h"
#include "CSSEditUtils.h"
#include "EditAction.h"
#include "EditorDOMPoint.h"
#include "EditorUtils.h"
#include "HTMLEditHelpers.h"
#include "HTMLEditUtils.h"
#include "TypeInState.h"  // for SpecifiedStyle
#include "WSRunObject.h"

#include "mozilla/Assertions.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/ContentIterator.h"
#include "mozilla/IntegerRange.h"
#include "mozilla/InternalMutationEvent.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"
#include "mozilla/OwningNonNull.h"
#include "mozilla/Preferences.h"
#include "mozilla/RangeUtils.h"
#include "mozilla/StaticPrefs_editor.h"  // for StaticPrefs::editor_*
#include "mozilla/TextComposition.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Unused.h"
#include "mozilla/dom/AncestorIterator.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/HTMLBRElement.h"
#include "mozilla/dom/RangeBinding.h"
#include "mozilla/dom/Selection.h"
#include "mozilla/dom/StaticRange.h"
#include "mozilla/mozalloc.h"
#include "nsAString.h"
#include "nsAlgorithm.h"
#include "nsAtom.h"
#include "nsCRT.h"
#include "nsCRTGlue.h"
#include "nsComponentManagerUtils.h"
#include "nsContentUtils.h"
#include "nsDebug.h"
#include "nsError.h"
#include "nsFrameSelection.h"
#include "nsGkAtoms.h"
#include "nsHTMLDocument.h"
#include "nsIContent.h"
#include "nsID.h"
#include "nsIFrame.h"
#include "nsINode.h"
#include "nsLiteralString.h"
#include "nsPrintfCString.h"
#include "nsRange.h"
#include "nsReadableUtils.h"
#include "nsString.h"
#include "nsStringFwd.h"
#include "nsStyledElement.h"
#include "nsTArray.h"
#include "nsTextNode.h"
#include "nsThreadUtils.h"
#include "nsUnicharUtils.h"

// Workaround for windows headers
#ifdef SetProp
#  undef SetProp
#endif

class nsISupports;

namespace mozilla {

using namespace dom;
using EmptyCheckOption = HTMLEditUtils::EmptyCheckOption;
using EmptyCheckOptions = HTMLEditUtils::EmptyCheckOptions;
using LeafNodeType = HTMLEditUtils::LeafNodeType;
using LeafNodeTypes = HTMLEditUtils::LeafNodeTypes;
using StyleDifference = HTMLEditUtils::StyleDifference;
using WalkTextOption = HTMLEditUtils::WalkTextOption;
using WalkTreeDirection = HTMLEditUtils::WalkTreeDirection;
using WalkTreeOption = HTMLEditUtils::WalkTreeOption;

/********************************************************
 *  first some helpful functors we will use
 ********************************************************/

static bool IsStyleCachePreservingSubAction(EditSubAction aEditSubAction) {
  switch (aEditSubAction) {
    case EditSubAction::eDeleteSelectedContent:
    case EditSubAction::eInsertLineBreak:
    case EditSubAction::eInsertParagraphSeparator:
    case EditSubAction::eCreateOrChangeList:
    case EditSubAction::eIndent:
    case EditSubAction::eOutdent:
    case EditSubAction::eSetOrClearAlignment:
    case EditSubAction::eCreateOrRemoveBlock:
    case EditSubAction::eMergeBlockContents:
    case EditSubAction::eRemoveList:
    case EditSubAction::eCreateOrChangeDefinitionListItem:
    case EditSubAction::eInsertElement:
    case EditSubAction::eInsertQuotation:
    case EditSubAction::eInsertQuotedText:
      return true;
    default:
      return false;
  }
}

template already_AddRefed<nsRange>
HTMLEditor::CreateRangeIncludingAdjuscentWhiteSpaces(
    const EditorDOMRange& aRange);
template already_AddRefed<nsRange>
HTMLEditor::CreateRangeIncludingAdjuscentWhiteSpaces(
    const EditorRawDOMRange& aRange);
template already_AddRefed<nsRange>
HTMLEditor::CreateRangeIncludingAdjuscentWhiteSpaces(
    const EditorDOMPoint& aStartPoint, const EditorDOMPoint& aEndPoint);
template already_AddRefed<nsRange>
HTMLEditor::CreateRangeIncludingAdjuscentWhiteSpaces(
    const EditorRawDOMPoint& aStartPoint, const EditorDOMPoint& aEndPoint);
template already_AddRefed<nsRange>
HTMLEditor::CreateRangeIncludingAdjuscentWhiteSpaces(
    const EditorDOMPoint& aStartPoint, const EditorRawDOMPoint& aEndPoint);
template already_AddRefed<nsRange>
HTMLEditor::CreateRangeIncludingAdjuscentWhiteSpaces(
    const EditorRawDOMPoint& aStartPoint, const EditorRawDOMPoint& aEndPoint);

nsresult HTMLEditor::InitEditorContentAndSelection() {
  MOZ_ASSERT(IsEditActionDataAvailable());

  nsresult rv = EditorBase::InitEditorContentAndSelection();
  if (NS_FAILED(rv)) {
    NS_WARNING("EditorBase::InitEditorContentAndSelection() failed");
    return rv;
  }

  Element* bodyOrDocumentElement = GetRoot();
  if (NS_WARN_IF(!bodyOrDocumentElement && !GetDocument())) {
    return NS_ERROR_FAILURE;
  }

  if (!bodyOrDocumentElement) {
    return NS_OK;
  }

  rv = InsertBRElementToEmptyListItemsAndTableCellsInRange(
      RawRangeBoundary(bodyOrDocumentElement, 0u),
      RawRangeBoundary(bodyOrDocumentElement,
                       bodyOrDocumentElement->GetChildCount()));
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::InsertBRElementToEmptyListItemsAndTableCellsInRange() "
      "failed, but ignored");
  return NS_OK;
}

void HTMLEditor::OnStartToHandleTopLevelEditSubAction(
    EditSubAction aTopLevelEditSubAction,
    nsIEditor::EDirection aDirectionOfTopLevelEditSubAction, ErrorResult& aRv) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(!aRv.Failed());

  EditorBase::OnStartToHandleTopLevelEditSubAction(
      aTopLevelEditSubAction, aDirectionOfTopLevelEditSubAction, aRv);

  MOZ_ASSERT(GetTopLevelEditSubAction() == aTopLevelEditSubAction);
  MOZ_ASSERT(GetDirectionOfTopLevelEditSubAction() ==
             aDirectionOfTopLevelEditSubAction);

  if (NS_WARN_IF(Destroyed())) {
    aRv.Throw(NS_ERROR_EDITOR_DESTROYED);
    return;
  }

  if (!mInitSucceeded) {
    return;  // We should do nothing if we're being initialized.
  }

  NS_WARNING_ASSERTION(
      !aRv.Failed(),
      "EditorBase::OnStartToHandleTopLevelEditSubAction() failed");

  // Remember where our selection was before edit action took place:
  const auto atCompositionStart =
      GetFirstIMESelectionStartPoint<EditorRawDOMPoint>();
  if (atCompositionStart.IsSet()) {
    // If there is composition string, let's remember current composition
    // range.
    TopLevelEditSubActionDataRef().mSelectedRange->StoreRange(
        atCompositionStart, GetLastIMESelectionEndPoint<EditorRawDOMPoint>());
  } else {
    // Get the selection location
    // XXX This may occur so that I think that we shouldn't throw exception
    //     in this case.
    if (NS_WARN_IF(!SelectionRef().RangeCount())) {
      aRv.Throw(NS_ERROR_UNEXPECTED);
      return;
    }
    if (const nsRange* range = SelectionRef().GetRangeAt(0)) {
      TopLevelEditSubActionDataRef().mSelectedRange->StoreRange(*range);
    }
  }

  // Register with range updater to track this as we perturb the doc
  RangeUpdaterRef().RegisterRangeItem(
      *TopLevelEditSubActionDataRef().mSelectedRange);

  // Remember current inline styles for deletion and normal insertion ops
  bool cacheInlineStyles;
  switch (aTopLevelEditSubAction) {
    case EditSubAction::eInsertText:
    case EditSubAction::eInsertTextComingFromIME:
    case EditSubAction::eDeleteSelectedContent:
      cacheInlineStyles = true;
      break;
    default:
      cacheInlineStyles =
          IsStyleCachePreservingSubAction(aTopLevelEditSubAction);
      break;
  }
  if (cacheInlineStyles) {
    nsCOMPtr<nsIContent> containerContent = nsIContent::FromNodeOrNull(
        aDirectionOfTopLevelEditSubAction == nsIEditor::eNext
            ? TopLevelEditSubActionDataRef().mSelectedRange->mEndContainer
            : TopLevelEditSubActionDataRef().mSelectedRange->mStartContainer);
    if (NS_WARN_IF(!containerContent)) {
      aRv.Throw(NS_ERROR_FAILURE);
      return;
    }
    nsresult rv = CacheInlineStyles(*containerContent);
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::CacheInlineStyles() failed");
      aRv.Throw(rv);
      return;
    }
  }

  // Stabilize the document against contenteditable count changes
  Document* document = GetDocument();
  if (NS_WARN_IF(!document)) {
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }
  if (document->GetEditingState() == Document::EditingState::eContentEditable) {
    document->ChangeContentEditableCount(nullptr, +1);
    TopLevelEditSubActionDataRef().mRestoreContentEditableCount = true;
  }

  // Check that selection is in subtree defined by body node
  nsresult rv = EnsureSelectionInBodyOrDocumentElement();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    aRv.Throw(NS_ERROR_EDITOR_DESTROYED);
    return;
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::EnsureSelectionInBodyOrDocumentElement() "
                       "failed, but ignored");
}

nsresult HTMLEditor::OnEndHandlingTopLevelEditSubAction() {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  nsresult rv;
  while (true) {
    if (NS_WARN_IF(Destroyed())) {
      rv = NS_ERROR_EDITOR_DESTROYED;
      break;
    }

    if (!mInitSucceeded) {
      rv = NS_OK;  // We should do nothing if we're being initialized.
      break;
    }

    // Do all the tricky stuff
    rv = OnEndHandlingTopLevelEditSubActionInternal();
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::OnEndHandlingTopLevelEditSubActionInternal() failied");
    // Perhaps, we need to do the following jobs even if the editor has been
    // destroyed since they adjust some states of HTML document but don't
    // modify the DOM tree nor Selection.

    // Free up selectionState range item
    if (TopLevelEditSubActionDataRef().mSelectedRange) {
      RangeUpdaterRef().DropRangeItem(
          *TopLevelEditSubActionDataRef().mSelectedRange);
    }

    // Reset the contenteditable count to its previous value
    if (TopLevelEditSubActionDataRef().mRestoreContentEditableCount) {
      Document* document = GetDocument();
      if (NS_WARN_IF(!document)) {
        rv = NS_ERROR_FAILURE;
        break;
      }
      if (document->GetEditingState() ==
          Document::EditingState::eContentEditable) {
        document->ChangeContentEditableCount(nullptr, -1);
      }
    }
    break;
  }
  DebugOnly<nsresult> rvIgnored =
      EditorBase::OnEndHandlingTopLevelEditSubAction();
  NS_WARNING_ASSERTION(
      NS_FAILED(rv) || NS_SUCCEEDED(rvIgnored),
      "EditorBase::OnEndHandlingTopLevelEditSubAction() failed, but ignored");
  MOZ_ASSERT(!GetTopLevelEditSubAction());
  MOZ_ASSERT(GetDirectionOfTopLevelEditSubAction() == eNone);
  return rv;
}

nsresult HTMLEditor::OnEndHandlingTopLevelEditSubActionInternal() {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  nsresult rv = EnsureSelectionInBodyOrDocumentElement();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::EnsureSelectionInBodyOrDocumentElement() "
                       "failed, but ignored");

  switch (GetTopLevelEditSubAction()) {
    case EditSubAction::eReplaceHeadWithHTMLSource:
    case EditSubAction::eCreatePaddingBRElementForEmptyEditor:
      return NS_OK;
    default:
      break;
  }

  if (TopLevelEditSubActionDataRef().mChangedRange->IsPositioned() &&
      GetTopLevelEditSubAction() != EditSubAction::eUndo &&
      GetTopLevelEditSubAction() != EditSubAction::eRedo) {
    // don't let any txns in here move the selection around behind our back.
    // Note that this won't prevent explicit selection setting from working.
    AutoTransactionsConserveSelection dontChangeMySelection(*this);

    {
      EditorDOMRange changedRange(
          *TopLevelEditSubActionDataRef().mChangedRange);
      if (changedRange.IsPositioned() &&
          changedRange.EnsureNotInNativeAnonymousSubtree()) {
        switch (GetTopLevelEditSubAction()) {
          case EditSubAction::eInsertText:
          case EditSubAction::eInsertTextComingFromIME:
          case EditSubAction::eInsertLineBreak:
          case EditSubAction::eInsertParagraphSeparator:
          case EditSubAction::eDeleteText: {
            // XXX We should investigate whether this is really needed because
            //     it seems that the following code does not handle the
            //     white-spaces.
            RefPtr<nsRange> extendedChangedRange =
                CreateRangeIncludingAdjuscentWhiteSpaces(changedRange);
            if (extendedChangedRange) {
              MOZ_ASSERT(extendedChangedRange->IsPositioned());
              // Use extended range temporarily.
              TopLevelEditSubActionDataRef().mChangedRange =
                  std::move(extendedChangedRange);
            }
            break;
          }
          default: {
            if (Element* editingHost = ComputeEditingHost()) {
              if (RefPtr<nsRange> extendedChangedRange = AutoRangeArray::
                      CreateRangeWrappingStartAndEndLinesContainingBoundaries(
                          changedRange, GetTopLevelEditSubAction(),
                          *editingHost)) {
                MOZ_ASSERT(extendedChangedRange->IsPositioned());
                // Use extended range temporarily.
                TopLevelEditSubActionDataRef().mChangedRange =
                    std::move(extendedChangedRange);
              }
              break;
            }
          }
        }
      }
    }

    // if we did a ranged deletion or handling backspace key, make sure we have
    // a place to put caret.
    // Note we only want to do this if the overall operation was deletion,
    // not if deletion was done along the way for
    // EditSubAction::eInsertHTMLSource, EditSubAction::eInsertText, etc.
    // That's why this is here rather than DeleteSelectionAsSubAction().
    // However, we shouldn't insert <br> elements if we've already removed
    // empty block parents because users may want to disappear the line by
    // the deletion.
    // XXX We should make HandleDeleteSelection() store expected container
    //     for handling this here since we cannot trust current selection is
    //     collapsed at deleted point.
    if (GetTopLevelEditSubAction() == EditSubAction::eDeleteSelectedContent &&
        TopLevelEditSubActionDataRef().mDidDeleteNonCollapsedRange &&
        !TopLevelEditSubActionDataRef().mDidDeleteEmptyParentBlocks) {
      const auto newCaretPosition =
          GetFirstSelectionStartPoint<EditorDOMPoint>();
      if (!newCaretPosition.IsSet()) {
        NS_WARNING("There was no selection range");
        return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
      }
      nsresult rv = InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary(
          newCaretPosition);
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "HTMLEditor::"
            "InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary() "
            "failed");
        return rv;
      }
    }

    // add in any needed <br>s, and remove any unneeded ones.
    nsresult rv = InsertBRElementToEmptyListItemsAndTableCellsInRange(
        TopLevelEditSubActionDataRef().mChangedRange->StartRef().AsRaw(),
        TopLevelEditSubActionDataRef().mChangedRange->EndRef().AsRaw());
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::InsertBRElementToEmptyListItemsAndTableCellsInRange()"
        " failed, but ignored");

    // merge any adjacent text nodes
    switch (GetTopLevelEditSubAction()) {
      case EditSubAction::eInsertText:
      case EditSubAction::eInsertTextComingFromIME:
        break;
      default: {
        nsresult rv = CollapseAdjacentTextNodes(
            MOZ_KnownLive(*TopLevelEditSubActionDataRef().mChangedRange));
        if (NS_WARN_IF(Destroyed())) {
          return NS_ERROR_EDITOR_DESTROYED;
        }
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::CollapseAdjacentTextNodes() failed");
          return rv;
        }
        break;
      }
    }

    // Clean up any empty nodes in the changed range unless they are inserted
    // intentionally.
    if (TopLevelEditSubActionDataRef().mNeedsToCleanUpEmptyElements) {
      nsresult rv = RemoveEmptyNodesIn(
          EditorDOMRange(*TopLevelEditSubActionDataRef().mChangedRange));
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::RemoveEmptyNodesIn() failed");
        return rv;
      }
    }

    // attempt to transform any unneeded nbsp's into spaces after doing various
    // operations
    switch (GetTopLevelEditSubAction()) {
      case EditSubAction::eDeleteSelectedContent:
        if (TopLevelEditSubActionDataRef().mDidNormalizeWhitespaces) {
          break;
        }
        [[fallthrough]];
      case EditSubAction::eInsertText:
      case EditSubAction::eInsertTextComingFromIME:
      case EditSubAction::eInsertLineBreak:
      case EditSubAction::eInsertParagraphSeparator:
      case EditSubAction::ePasteHTMLContent:
      case EditSubAction::eInsertHTMLSource: {
        // TODO: Temporarily, WhiteSpaceVisibilityKeeper replaces ASCII
        //       white-spaces with NPSPs and then, we'll replace them with ASCII
        //       white-spaces here.  We should avoid this overwriting things as
        //       far as possible because replacing characters in text nodes
        //       causes running mutation event listeners which are really
        //       expensive.
        // Adjust end of composition string if there is composition string.
        auto pointToAdjust = GetLastIMESelectionEndPoint<EditorDOMPoint>();
        if (!pointToAdjust.IsInContentNode()) {
          // Otherwise, adjust current selection start point.
          pointToAdjust = GetFirstSelectionStartPoint<EditorDOMPoint>();
          if (NS_WARN_IF(!pointToAdjust.IsInContentNode())) {
            return NS_ERROR_FAILURE;
          }
        }
        if (EditorUtils::IsEditableContent(
                *pointToAdjust.ContainerAs<nsIContent>(), EditorType::HTML)) {
          AutoTrackDOMPoint trackPointToAdjust(RangeUpdaterRef(),
                                               &pointToAdjust);
          nsresult rv =
              WhiteSpaceVisibilityKeeper::NormalizeVisibleWhiteSpacesAt(
                  *this, pointToAdjust);
          if (NS_FAILED(rv)) {
            NS_WARNING(
                "WhiteSpaceVisibilityKeeper::NormalizeVisibleWhiteSpacesAt() "
                "failed");
            return rv;
          }
        }

        // also do this for original selection endpoints.
        // XXX Hmm, if `NormalizeVisibleWhiteSpacesAt()` runs mutation event
        //     listener and that causes changing `mSelectedRange`, what we
        //     should do?
        if (NS_WARN_IF(!TopLevelEditSubActionDataRef()
                            .mSelectedRange->IsPositioned())) {
          return NS_ERROR_FAILURE;
        }

        EditorDOMPoint atStart =
            TopLevelEditSubActionDataRef().mSelectedRange->StartPoint();
        if (atStart != pointToAdjust && atStart.IsInContentNode() &&
            EditorUtils::IsEditableContent(*atStart.ContainerAs<nsIContent>(),
                                           EditorType::HTML)) {
          AutoTrackDOMPoint trackPointToAdjust(RangeUpdaterRef(),
                                               &pointToAdjust);
          AutoTrackDOMPoint trackStartPoint(RangeUpdaterRef(), &atStart);
          nsresult rv =
              WhiteSpaceVisibilityKeeper::NormalizeVisibleWhiteSpacesAt(
                  *this, atStart);
          if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
            return NS_ERROR_EDITOR_DESTROYED;
          }
          NS_WARNING_ASSERTION(
              NS_SUCCEEDED(rv),
              "WhiteSpaceVisibilityKeeper::NormalizeVisibleWhiteSpacesAt() "
              "failed, but ignored");
        }
        // we only need to handle old selection endpoint if it was different
        // from start
        EditorDOMPoint atEnd =
            TopLevelEditSubActionDataRef().mSelectedRange->EndPoint();
        if (!TopLevelEditSubActionDataRef().mSelectedRange->Collapsed() &&
            atEnd != pointToAdjust && atEnd != atStart &&
            atEnd.IsInContentNode() &&
            EditorUtils::IsEditableContent(*atEnd.ContainerAs<nsIContent>(),
                                           EditorType::HTML)) {
          nsresult rv =
              WhiteSpaceVisibilityKeeper::NormalizeVisibleWhiteSpacesAt(*this,
                                                                        atEnd);
          if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
            return NS_ERROR_EDITOR_DESTROYED;
          }
          NS_WARNING_ASSERTION(
              NS_SUCCEEDED(rv),
              "WhiteSpaceVisibilityKeeper::NormalizeVisibleWhiteSpacesAt() "
              "failed, but ignored");
        }
        break;
      }
      default:
        break;
    }

    // Adjust selection for insert text, html paste, and delete actions if
    // we haven't removed new empty blocks.  Note that if empty block parents
    // are removed, Selection should've been adjusted by the method which
    // did it.
    if (!TopLevelEditSubActionDataRef().mDidDeleteEmptyParentBlocks &&
        SelectionRef().IsCollapsed()) {
      switch (GetTopLevelEditSubAction()) {
        case EditSubAction::eInsertText:
        case EditSubAction::eInsertTextComingFromIME:
        case EditSubAction::eDeleteSelectedContent:
        case EditSubAction::eInsertLineBreak:
        case EditSubAction::eInsertParagraphSeparator:
        case EditSubAction::ePasteHTMLContent:
        case EditSubAction::eInsertHTMLSource:
          // XXX AdjustCaretPositionAndEnsurePaddingBRElement() intentionally
          //     does not create padding `<br>` element for empty editor.
          //     Investigate which is better that whether this should does it
          //     or wait MaybeCreatePaddingBRElementForEmptyEditor().
          rv = AdjustCaretPositionAndEnsurePaddingBRElement(
              GetDirectionOfTopLevelEditSubAction());
          if (NS_FAILED(rv)) {
            NS_WARNING(
                "HTMLEditor::AdjustCaretPositionAndEnsurePaddingBRElement() "
                "failed");
            return rv;
          }
          break;
        default:
          break;
      }
    }

    // check for any styles which were removed inappropriately
    bool reapplyCachedStyle;
    switch (GetTopLevelEditSubAction()) {
      case EditSubAction::eInsertText:
      case EditSubAction::eInsertTextComingFromIME:
      case EditSubAction::eDeleteSelectedContent:
        reapplyCachedStyle = true;
        break;
      default:
        reapplyCachedStyle =
            IsStyleCachePreservingSubAction(GetTopLevelEditSubAction());
        break;
    }

    // If the selection is in empty inline HTML elements, we should delete
    // them unless it's inserted intentionally.
    if (mPlaceholderBatch &&
        TopLevelEditSubActionDataRef().mNeedsToCleanUpEmptyElements &&
        SelectionRef().IsCollapsed() && SelectionRef().GetFocusNode()) {
      RefPtr<Element> mostDistantEmptyInlineAncestor = nullptr;
      for (Element* ancestor :
           SelectionRef().GetFocusNode()->InclusiveAncestorsOfType<Element>()) {
        if (!ancestor->IsHTMLElement() ||
            !HTMLEditUtils::IsRemovableFromParentNode(*ancestor) ||
            !HTMLEditUtils::IsEmptyInlineContainer(
                *ancestor, {EmptyCheckOption::TreatSingleBRElementAsVisible})) {
          break;
        }
        mostDistantEmptyInlineAncestor = ancestor;
      }
      if (mostDistantEmptyInlineAncestor) {
        nsresult rv =
            DeleteNodeWithTransaction(*mostDistantEmptyInlineAncestor);
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "EditorBase::DeleteNodeWithTransaction() failed at deleting "
              "empty inline ancestors");
          return rv;
        }
      }
    }

    // But the cached inline styles should be restored from type-in-state later.
    if (reapplyCachedStyle) {
      DebugOnly<nsresult> rvIgnored = mTypeInState->UpdateSelState(*this);
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                           "TypeInState::UpdateSelState() failed, but ignored");
      rvIgnored = ReapplyCachedStyles();
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rvIgnored),
          "HTMLEditor::ReapplyCachedStyles() failed, but ignored");
      TopLevelEditSubActionDataRef().mCachedInlineStyles->Clear();
    }
  }

  rv = HandleInlineSpellCheck(
      TopLevelEditSubActionDataRef().mSelectedRange->StartPoint(),
      TopLevelEditSubActionDataRef().mChangedRange);
  if (NS_FAILED(rv)) {
    NS_WARNING("EditorBase::HandleInlineSpellCheck() failed");
    return rv;
  }

  // detect empty doc
  // XXX Need to investigate when the padding <br> element is removed because
  //     I don't see the <br> element with testing manually.  If it won't be
  //     used, we can get rid of this cost.
  rv = MaybeCreatePaddingBRElementForEmptyEditor();
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "EditorBase::MaybeCreatePaddingBRElementForEmptyEditor() failed");
    return rv;
  }

  // adjust selection HINT if needed
  if (!TopLevelEditSubActionDataRef().mDidExplicitlySetInterLine &&
      SelectionRef().IsCollapsed()) {
    SetSelectionInterlinePosition();
  }

  return NS_OK;
}

EditActionResult HTMLEditor::CanHandleHTMLEditSubAction() const {
  MOZ_ASSERT(IsEditActionDataAvailable());

  if (NS_WARN_IF(Destroyed())) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }

  // If there is not selection ranges, we should ignore the result.
  if (!SelectionRef().RangeCount()) {
    return EditActionCanceled();
  }

  const nsRange* range = SelectionRef().GetRangeAt(0);
  nsINode* selStartNode = range->GetStartContainer();
  if (NS_WARN_IF(!selStartNode) || NS_WARN_IF(!selStartNode->IsContent())) {
    return EditActionResult(NS_ERROR_FAILURE);
  }

  if (!HTMLEditUtils::IsSimplyEditableNode(*selStartNode) ||
      HTMLEditUtils::IsNonEditableReplacedContent(*selStartNode->AsContent())) {
    return EditActionCanceled();
  }

  nsINode* selEndNode = range->GetEndContainer();
  if (NS_WARN_IF(!selEndNode) || NS_WARN_IF(!selEndNode->IsContent())) {
    return EditActionResult(NS_ERROR_FAILURE);
  }

  if (selStartNode == selEndNode) {
    return EditActionIgnored();
  }

  if (!HTMLEditUtils::IsSimplyEditableNode(*selEndNode) ||
      HTMLEditUtils::IsNonEditableReplacedContent(*selEndNode->AsContent())) {
    return EditActionCanceled();
  }

  // XXX What does it mean the common ancestor is editable?  I have no idea.
  //     It should be in same (active) editing host, and even if it's editable,
  //     there may be non-editable contents in the range.
  nsINode* commonAncestor = range->GetClosestCommonInclusiveAncestor();
  if (!commonAncestor) {
    NS_WARNING(
        "AbstractRange::GetClosestCommonInclusiveAncestor() returned nullptr");
    return EditActionResult(NS_ERROR_FAILURE);
  }
  return HTMLEditUtils::IsSimplyEditableNode(*commonAncestor)
             ? EditActionIgnored()
             : EditActionCanceled();
}

MOZ_CAN_RUN_SCRIPT static nsStaticAtom& MarginPropertyAtomForIndent(
    nsIContent& aContent) {
  nsAutoString direction;
  DebugOnly<nsresult> rvIgnored = CSSEditUtils::GetComputedProperty(
      aContent, *nsGkAtoms::direction, direction);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                       "CSSEditUtils::GetComputedProperty(nsGkAtoms::direction)"
                       " failed, but ignored");
  return direction.EqualsLiteral("rtl") ? *nsGkAtoms::marginRight
                                        : *nsGkAtoms::marginLeft;
}

nsresult HTMLEditor::EnsureCaretNotAfterInvisibleBRElement() {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(SelectionRef().IsCollapsed());

  // If we are after a padding `<br>` element for empty last line in the same
  // block, then move selection to be before it
  const nsRange* firstRange = SelectionRef().GetRangeAt(0);
  if (NS_WARN_IF(!firstRange)) {
    return NS_ERROR_FAILURE;
  }

  EditorRawDOMPoint atSelectionStart(firstRange->StartRef());
  if (NS_WARN_IF(!atSelectionStart.IsSet())) {
    return NS_ERROR_FAILURE;
  }
  MOZ_ASSERT(atSelectionStart.IsSetAndValid());

  if (!atSelectionStart.IsInContentNode()) {
    return NS_OK;
  }

  Element* editingHost = ComputeEditingHost();
  if (!editingHost) {
    NS_WARNING(
        "HTMLEditor::EnsureCaretNotAfterInvisibleBRElement() did nothing "
        "because of no editing host");
    return NS_OK;
  }

  nsIContent* previousBRElement =
      HTMLEditUtils::GetPreviousContent(atSelectionStart, {}, editingHost);
  if (!previousBRElement || !previousBRElement->IsHTMLElement(nsGkAtoms::br) ||
      !previousBRElement->GetParent() ||
      !EditorUtils::IsEditableContent(*previousBRElement->GetParent(),
                                      EditorType::HTML) ||
      !HTMLEditUtils::IsInvisibleBRElement(*previousBRElement)) {
    return NS_OK;
  }

  const RefPtr<const Element> blockElementAtSelectionStart =
      HTMLEditUtils::GetInclusiveAncestorElement(
          *atSelectionStart.ContainerAs<nsIContent>(),
          HTMLEditUtils::ClosestBlockElement);
  const RefPtr<const Element> parentBlockElementOfBRElement =
      HTMLEditUtils::GetAncestorElement(*previousBRElement,
                                        HTMLEditUtils::ClosestBlockElement);

  if (!blockElementAtSelectionStart ||
      blockElementAtSelectionStart != parentBlockElementOfBRElement) {
    return NS_OK;
  }

  // If we are here then the selection is right after a padding <br>
  // element for empty last line that is in the same block as the
  // selection.  We need to move the selection start to be before the
  // padding <br> element.
  EditorRawDOMPoint atInvisibleBRElement(previousBRElement);
  nsresult rv = CollapseSelectionTo(atInvisibleBRElement);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::CollapseSelectionTo() failed");
  return rv;
}

nsresult HTMLEditor::MaybeCreatePaddingBRElementForEmptyEditor() {
  MOZ_ASSERT(IsEditActionDataAvailable());

  if (mPaddingBRElementForEmptyEditor) {
    return NS_OK;
  }

  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this, EditSubAction::eCreatePaddingBRElementForEmptyEditor,
      nsIEditor::eNone, ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return ignoredError.StealNSResult();
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "TextEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");
  ignoredError.SuppressException();

  RefPtr<Element> rootElement = GetRoot();
  if (!rootElement) {
    return NS_OK;
  }

  // Now we've got the body element. Iterate over the body element's children,
  // looking for editable content. If no editable content is found, insert the
  // padding <br> element.
  EditorType editorType = GetEditorType();
  bool isRootEditable =
      EditorUtils::IsEditableContent(*rootElement, editorType);
  for (nsIContent* rootChild = rootElement->GetFirstChild(); rootChild;
       rootChild = rootChild->GetNextSibling()) {
    if (EditorUtils::IsPaddingBRElementForEmptyEditor(*rootChild) ||
        !isRootEditable ||
        EditorUtils::IsEditableContent(*rootChild, editorType) ||
        HTMLEditUtils::IsBlockElement(*rootChild)) {
      return NS_OK;
    }
  }

  // Skip adding the padding <br> element for empty editor if body
  // is read-only.
  if (IsHTMLEditor() && !HTMLEditUtils::IsSimplyEditableNode(*rootElement)) {
    return NS_OK;
  }

  // Create a br.
  RefPtr<Element> newBRElement = CreateHTMLContent(nsGkAtoms::br);
  if (NS_WARN_IF(Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  if (NS_WARN_IF(!newBRElement)) {
    return NS_ERROR_FAILURE;
  }

  mPaddingBRElementForEmptyEditor =
      static_cast<HTMLBRElement*>(newBRElement.get());

  // Give it a special attribute.
  newBRElement->SetFlags(NS_PADDING_FOR_EMPTY_EDITOR);

  // Put the node in the document.
  CreateElementResult insertBRElementResult =
      InsertNodeWithTransaction<Element>(*newBRElement,
                                         EditorDOMPoint(rootElement, 0u));
  if (insertBRElementResult.isErr()) {
    NS_WARNING("EditorBase::InsertNodeWithTransaction() failed");
    return insertBRElementResult.unwrapErr();
  }

  // Set selection.
  insertBRElementResult.IgnoreCaretPointSuggestion();
  nsresult rv = CollapseSelectionToStartOf(*rootElement);
  if (MOZ_UNLIKELY(rv == NS_ERROR_EDITOR_DESTROYED)) {
    NS_WARNING(
        "EditorBase::CollapseSelectionToStartOf() caused destroying the "
        "editor");
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "EditorBase::CollapseSelectionToStartOf() failed, but ignored");
  return NS_OK;
}

nsresult HTMLEditor::EnsureNoPaddingBRElementForEmptyEditor() {
  MOZ_ASSERT(IsEditActionDataAvailable());

  if (!mPaddingBRElementForEmptyEditor) {
    return NS_OK;
  }

  // If we're an HTML editor, a mutation event listener may recreate padding
  // <br> element for empty editor again during the call of
  // DeleteNodeWithTransaction().  So, move it first.
  RefPtr<HTMLBRElement> paddingBRElement(
      std::move(mPaddingBRElementForEmptyEditor));
  nsresult rv = DeleteNodeWithTransaction(*paddingBRElement);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::DeleteNodeWithTransaction() failed");
  return rv;
}

nsresult HTMLEditor::ReflectPaddingBRElementForEmptyEditor() {
  if (NS_WARN_IF(!mRootElement)) {
    NS_WARNING("Failed to handle padding BR element due to no root element");
    return NS_ERROR_FAILURE;
  }
  // The idea here is to see if the magic empty node has suddenly reappeared. If
  // it has, set our state so we remember it. There is a tradeoff between doing
  // here and at redo, or doing it everywhere else that might care.  Since undo
  // and redo are relatively rare, it makes sense to take the (small)
  // performance hit here.
  nsIContent* firstLeafChild = HTMLEditUtils::GetFirstLeafContent(
      *mRootElement, {LeafNodeType::OnlyLeafNode});
  if (firstLeafChild &&
      EditorUtils::IsPaddingBRElementForEmptyEditor(*firstLeafChild)) {
    mPaddingBRElementForEmptyEditor =
        static_cast<HTMLBRElement*>(firstLeafChild);
  } else {
    mPaddingBRElementForEmptyEditor = nullptr;
  }
  return NS_OK;
}

nsresult HTMLEditor::PrepareInlineStylesForCaret() {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(SelectionRef().IsCollapsed());

  // XXX This method works with the top level edit sub-action, but this
  //     must be wrong if we are handling nested edit action.

  if (TopLevelEditSubActionDataRef().mDidDeleteSelection) {
    switch (GetTopLevelEditSubAction()) {
      case EditSubAction::eInsertText:
      case EditSubAction::eInsertTextComingFromIME:
      case EditSubAction::eDeleteSelectedContent: {
        nsresult rv = ReapplyCachedStyles();
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::ReapplyCachedStyles() failed");
          return rv;
        }
        break;
      }
      default:
        break;
    }
  }
  // For most actions we want to clear the cached styles, but there are
  // exceptions
  if (!IsStyleCachePreservingSubAction(GetTopLevelEditSubAction())) {
    TopLevelEditSubActionDataRef().mCachedInlineStyles->Clear();
  }
  return NS_OK;
}

EditActionResult HTMLEditor::HandleInsertText(
    EditSubAction aEditSubAction, const nsAString& aInsertionString,
    SelectionHandling aSelectionHandling) {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(aEditSubAction == EditSubAction::eInsertText ||
             aEditSubAction == EditSubAction::eInsertTextComingFromIME);
  MOZ_ASSERT_IF(aSelectionHandling == SelectionHandling::Ignore,
                aEditSubAction == EditSubAction::eInsertTextComingFromIME);

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  UndefineCaretBidiLevel();

  // If the selection isn't collapsed, delete it.  Don't delete existing inline
  // tags, because we're hopefully going to insert text (bug 787432).
  if (!SelectionRef().IsCollapsed() &&
      aSelectionHandling == SelectionHandling::Delete) {
    nsresult rv =
        DeleteSelectionAsSubAction(nsIEditor::eNone, nsIEditor::eNoStrip);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "EditorBase::DeleteSelectionAsSubAction(nsIEditor::eNone, "
          "nsIEditor::eNoStrip) failed");
      return EditActionHandled(rv);
    }
  }

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (NS_SUCCEEDED(rv) && SelectionRef().IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterInvisibleBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterInvisibleBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  RefPtr<Document> document = GetDocument();
  if (NS_WARN_IF(!document)) {
    return EditActionHandled(NS_ERROR_FAILURE);
  }

  const RefPtr<Element> editingHost = ComputeEditingHost(
      GetDocument()->IsXMLDocument() ? LimitInBodyElement::No
                                     : LimitInBodyElement::Yes);
  if (NS_WARN_IF(!editingHost)) {
    return EditActionHandled(NS_ERROR_FAILURE);
  }

  auto pointToInsert = GetFirstSelectionStartPoint<EditorDOMPoint>();
  if (MOZ_UNLIKELY(!pointToInsert.IsSet())) {
    return EditActionHandled(NS_ERROR_FAILURE);
  }

  // for every property that is set, insert a new inline style node
  Result<EditorDOMPoint, nsresult> setStyleResult =
      CreateStyleForInsertText(pointToInsert);
  if (MOZ_UNLIKELY(setStyleResult.isErr())) {
    NS_WARNING("HTMLEditor::CreateStyleForInsertText() failed");
    return EditActionHandled(setStyleResult.unwrapErr());
  }
  if (setStyleResult.inspect().IsSet()) {
    pointToInsert = setStyleResult.unwrap();
  }

  if (NS_WARN_IF(!pointToInsert.IsSetAndValid()) ||
      NS_WARN_IF(!pointToInsert.IsInContentNode())) {
    return EditActionHandled(NS_ERROR_FAILURE);
  }
  MOZ_ASSERT(pointToInsert.IsSetAndValid());

  // If the point is not in an element which can contain text nodes, climb up
  // the DOM tree.
  if (!pointToInsert.IsInTextNode()) {
    while (!HTMLEditUtils::CanNodeContain(*pointToInsert.GetContainer(),
                                          *nsGkAtoms::textTagName)) {
      if (NS_WARN_IF(pointToInsert.GetContainer() == editingHost) ||
          NS_WARN_IF(!pointToInsert.GetContainerParentAs<nsIContent>())) {
        NS_WARNING("Selection start point couldn't have text nodes");
        return EditActionHandled(NS_ERROR_FAILURE);
      }
      pointToInsert.Set(pointToInsert.ContainerAs<nsIContent>());
    }
  }

  if (aEditSubAction == EditSubAction::eInsertTextComingFromIME) {
    auto compositionStartPoint =
        GetFirstIMESelectionStartPoint<EditorDOMPoint>();
    if (!compositionStartPoint.IsSet()) {
      compositionStartPoint = pointToInsert;
    }

    if (aInsertionString.IsEmpty()) {
      // Right now the WhiteSpaceVisibilityKeeper code bails on empty strings,
      // but IME needs the InsertTextWithTransaction() call to still happen
      // since empty strings are meaningful there.
      Result<EditorDOMPoint, nsresult> insertTextResult =
          InsertTextWithTransaction(*document, aInsertionString,
                                    compositionStartPoint);
      if (MOZ_UNLIKELY(insertTextResult.isErr())) {
        NS_WARNING("HTMLEditor::InsertTextWithTransaction() failed");
        return EditActionResult(insertTextResult.unwrapErr());
      }
      return EditActionHandled();
    }

    auto compositionEndPoint = GetLastIMESelectionEndPoint<EditorDOMPoint>();
    if (!compositionEndPoint.IsSet()) {
      compositionEndPoint = compositionStartPoint;
    }
    Result<EditorDOMPoint, nsresult> replaceTextResult =
        WhiteSpaceVisibilityKeeper::ReplaceText(
            *this, aInsertionString,
            EditorDOMRange(compositionStartPoint, compositionEndPoint));
    if (MOZ_UNLIKELY(replaceTextResult.isErr())) {
      NS_WARNING("WhiteSpaceVisibilityKeeper::ReplaceText() failed");
      return EditActionHandled(replaceTextResult.unwrapErr());
    }

    compositionStartPoint = GetFirstIMESelectionStartPoint<EditorDOMPoint>();
    compositionEndPoint = GetLastIMESelectionEndPoint<EditorDOMPoint>();
    if (NS_WARN_IF(!compositionStartPoint.IsSet()) ||
        NS_WARN_IF(!compositionEndPoint.IsSet())) {
      // Mutation event listener has changed the DOM tree...
      return EditActionHandled();
    }
    nsresult rv = TopLevelEditSubActionDataRef().mChangedRange->SetStartAndEnd(
        compositionStartPoint.ToRawRangeBoundary(),
        compositionEndPoint.ToRawRangeBoundary());
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "nsRange::SetStartAndEnd() failed");
    return EditActionHandled(rv);
  }

  MOZ_ASSERT(aEditSubAction == EditSubAction::eInsertText);

  // find where we are
  EditorDOMPoint currentPoint(pointToInsert);

  // is our text going to be PREformatted?
  // We remember this so that we know how to handle tabs.
  const bool isWhiteSpaceCollapsible = !EditorUtils::IsWhiteSpacePreformatted(
      *pointToInsert.ContainerAs<nsIContent>());

  // turn off the edit listener: we know how to
  // build the "doc changed range" ourselves, and it's
  // must faster to do it once here than to track all
  // the changes one at a time.
  AutoRestore<bool> disableListener(
      EditSubActionDataRef().mAdjustChangedRangeFromListener);
  EditSubActionDataRef().mAdjustChangedRangeFromListener = false;

  // don't change my selection in subtransactions
  AutoTransactionsConserveSelection dontChangeMySelection(*this);
  int32_t pos = 0;
  constexpr auto newlineStr = NS_LITERAL_STRING_FROM_CSTRING(LFSTR);

  {
    AutoTrackDOMPoint tracker(RangeUpdaterRef(), &pointToInsert);

    // for efficiency, break out the pre case separately.  This is because
    // its a lot cheaper to search the input string for only newlines than
    // it is to search for both tabs and newlines.
    if (!isWhiteSpaceCollapsible || IsInPlaintextMode()) {
      while (pos != -1 &&
             pos < AssertedCast<int32_t>(aInsertionString.Length())) {
        int32_t oldPos = pos;
        int32_t subStrLen;
        pos = aInsertionString.FindChar(nsCRT::LF, oldPos);

        if (pos != -1) {
          subStrLen = pos - oldPos;
          // if first char is newline, then use just it
          if (!subStrLen) {
            subStrLen = 1;
          }
        } else {
          subStrLen = aInsertionString.Length() - oldPos;
          pos = aInsertionString.Length();
        }

        nsDependentSubstring subStr(aInsertionString, oldPos, subStrLen);

        // is it a return?
        if (subStr.Equals(newlineStr)) {
          CreateElementResult insertBRElementResult =
              InsertBRElement(WithTransaction::Yes, currentPoint);
          if (insertBRElementResult.isErr()) {
            NS_WARNING(
                "HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
            return EditActionHandled(insertBRElementResult.unwrapErr());
          }
          // We don't want to update selection here because we've blocked
          // InsertNodeTransaction updating selection with
          // dontChangeMySelection.
          insertBRElementResult.IgnoreCaretPointSuggestion();
          MOZ_ASSERT(!AllowsTransactionsToChangeSelection());

          pos++;
          RefPtr<Element> brElement = insertBRElementResult.UnwrapNewNode();
          if (brElement->GetNextSibling()) {
            pointToInsert.Set(brElement->GetNextSibling());
          } else {
            pointToInsert.SetToEndOf(currentPoint.GetContainer());
          }
          // XXX In most cases, pointToInsert and currentPoint are same here.
          //     But if the <br> element has been moved to different point by
          //     mutation observer, those points become different.
          currentPoint.SetAfter(brElement);
          NS_WARNING_ASSERTION(currentPoint.IsSet(),
                               "Failed to set after the <br> element");
          NS_WARNING_ASSERTION(currentPoint == pointToInsert,
                               "Perhaps, <br> element position has been moved "
                               "to different point "
                               "by mutation observer");
        } else {
          Result<EditorDOMPoint, nsresult> insertTextResult =
              InsertTextWithTransaction(*document, subStr, currentPoint);
          if (MOZ_UNLIKELY(insertTextResult.isErr())) {
            NS_WARNING("HTMLEditor::InsertTextWithTransaction() failed");
            return EditActionHandled(insertTextResult.unwrapErr());
          }
          currentPoint = insertTextResult.inspect();
          pointToInsert = insertTextResult.unwrap();
        }
      }
    } else {
      constexpr auto tabStr = u"\t"_ns;
      constexpr auto spacesStr = u"    "_ns;
      nsAutoString insertionString(aInsertionString);  // For FindCharInSet().
      while (pos != -1 &&
             pos < AssertedCast<int32_t>(insertionString.Length())) {
        int32_t oldPos = pos;
        int32_t subStrLen;
        pos = insertionString.FindCharInSet(u"\t\n", oldPos);

        if (pos != -1) {
          subStrLen = pos - oldPos;
          // if first char is newline, then use just it
          if (!subStrLen) {
            subStrLen = 1;
          }
        } else {
          subStrLen = insertionString.Length() - oldPos;
          pos = insertionString.Length();
        }

        nsDependentSubstring subStr(insertionString, oldPos, subStrLen);

        // is it a tab?
        if (subStr.Equals(tabStr)) {
          Result<EditorDOMPoint, nsresult> insertTextResult =
              WhiteSpaceVisibilityKeeper::InsertText(*this, spacesStr,
                                                     currentPoint);
          if (MOZ_UNLIKELY(insertTextResult.isErr())) {
            NS_WARNING("WhiteSpaceVisibilityKeeper::InsertText() failed");
            return EditActionHandled(insertTextResult.unwrapErr());
          }
          pos++;
          MOZ_ASSERT(insertTextResult.inspect().IsSet());
          currentPoint = insertTextResult.inspect();
          pointToInsert = insertTextResult.unwrap();
        }
        // is it a return?
        else if (subStr.Equals(newlineStr)) {
          CreateElementResult insertBRElementResult =
              WhiteSpaceVisibilityKeeper::InsertBRElement(*this, currentPoint,
                                                          *editingHost);
          if (insertBRElementResult.isErr()) {
            NS_WARNING("WhiteSpaceVisibilityKeeper::InsertBRElement() failed");
            return EditActionHandled(insertBRElementResult.unwrapErr());
          }
          // TODO: Some methods called for handling non-preformatted text use
          //       ComputeEditingHost().  Therefore, they depend on the latest
          //       selection.  So we cannot skip updating selection here.
          nsresult rv = insertBRElementResult.SuggestCaretPointTo(
              *this, {SuggestCaret::OnlyIfHasSuggestion,
                      SuggestCaret::OnlyIfTransactionsAllowedToDoIt,
                      SuggestCaret::AndIgnoreTrivialError});
          if (NS_FAILED(rv)) {
            NS_WARNING("CareateElementResult::SuggestCaretPointTo() failed");
            return EditActionHandled(rv);
          }
          NS_WARNING_ASSERTION(
              rv != NS_SUCCESS_EDITOR_BUT_IGNORED_TRIVIAL_ERROR,
              "CreateElementResult::SuggestCaretPointTo() failed, but ignored");
          pos++;
          RefPtr<Element> newBRElement = insertBRElementResult.UnwrapNewNode();
          MOZ_DIAGNOSTIC_ASSERT(newBRElement);
          if (newBRElement->GetNextSibling()) {
            pointToInsert.Set(newBRElement->GetNextSibling());
          } else {
            pointToInsert.SetToEndOf(currentPoint.GetContainer());
          }
          currentPoint.SetAfter(newBRElement);
          NS_WARNING_ASSERTION(currentPoint.IsSet(),
                               "Failed to set after the new <br> element");
          // XXX If the newBRElement has been moved or removed by mutation
          //     observer, we hit this assert.  We need to check if
          //     newBRElement is in expected point, though, we must have
          //     a lot of same bugs...
          NS_WARNING_ASSERTION(
              currentPoint == pointToInsert,
              "Perhaps, newBRElement has been moved or removed unexpectedly");
        } else {
          Result<EditorDOMPoint, nsresult> insertTextResult =
              WhiteSpaceVisibilityKeeper::InsertText(*this, subStr,
                                                     currentPoint);
          if (MOZ_UNLIKELY(insertTextResult.isErr())) {
            NS_WARNING("WhiteSpaceVisibilityKeeper::InsertText() failed");
            return EditActionHandled(insertTextResult.unwrapErr());
          }
          MOZ_ASSERT(insertTextResult.inspect().IsSet());
          currentPoint = insertTextResult.inspect();
          pointToInsert = insertTextResult.unwrap();
        }
      }
    }

    // After this block, pointToInsert is updated by AutoTrackDOMPoint.
  }

  if (currentPoint.IsSet()) {
    currentPoint.SetInterlinePosition(InterlinePosition::EndOfLine);
    nsresult rv = CollapseSelectionTo(currentPoint);
    if (MOZ_UNLIKELY(rv == NS_ERROR_EDITOR_DESTROYED)) {
      NS_WARNING(
          "EditorBase::CollapseSelectionTo() caused destroying the editor");
      return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "Selection::Collapse() failed, but ignored");

    // manually update the doc changed range so that AfterEdit will clean up
    // the correct portion of the document.
    rv = TopLevelEditSubActionDataRef().mChangedRange->SetStartAndEnd(
        pointToInsert.ToRawRangeBoundary(), currentPoint.ToRawRangeBoundary());
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "nsRange::SetStartAndEnd() failed");
    return EditActionHandled(rv);
  }

  DebugOnly<nsresult> rvIgnored =
      SelectionRef().SetInterlinePosition(InterlinePosition::EndOfLine);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                       "Selection::SetInterlinePosition(InterlinePosition::"
                       "EndOfLine) failed, but ignored");
  rv = TopLevelEditSubActionDataRef().mChangedRange->CollapseTo(pointToInsert);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "nsRange::CollapseTo() failed");
  return EditActionHandled(rv);
}

nsresult HTMLEditor::InsertLineBreakAsSubAction() {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(!IsSelectionRangeContainerNotContent());

  if (NS_WARN_IF(!mInitSucceeded)) {
    return NS_ERROR_NOT_INITIALIZED;
  }

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result.Rv();
  }

  // XXX This may be called by execCommand() with "insertLineBreak".
  //     In such case, naming the transaction "TypingTxnName" is odd.
  AutoPlaceholderBatch treatAsOneTransaction(*this, *nsGkAtoms::TypingTxnName,
                                             ScrollSelectionIntoView::Yes,
                                             __FUNCTION__);

  // calling it text insertion to trigger moz br treatment by rules
  // XXX Why do we use EditSubAction::eInsertText here?  Looks like
  //     EditSubAction::eInsertLineBreak or EditSubAction::eInsertNode
  //     is better.
  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this, EditSubAction::eInsertText, nsIEditor::eNext, ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return ignoredError.StealNSResult();
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  UndefineCaretBidiLevel();

  // If the selection isn't collapsed, delete it.
  if (!SelectionRef().IsCollapsed()) {
    nsresult rv =
        DeleteSelectionAsSubAction(nsIEditor::eNone, nsIEditor::eStrip);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "EditorBase::DeleteSelectionAsSubAction(eNone, eStrip) failed");
      return rv;
    }
  }

  const nsRange* firstRange = SelectionRef().GetRangeAt(0);
  if (NS_WARN_IF(!firstRange)) {
    return NS_ERROR_FAILURE;
  }

  EditorDOMPoint atStartOfSelection(firstRange->StartRef());
  if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
    return NS_ERROR_FAILURE;
  }
  MOZ_ASSERT(atStartOfSelection.IsSetAndValid());

  RefPtr<Element> editingHost = ComputeEditingHost();
  if (NS_WARN_IF(!editingHost)) {
    return NS_ERROR_FAILURE;
  }

  // For backward compatibility, we should not insert a linefeed if
  // paragraph separator is set to "br" which is Gecko-specific mode.
  if (GetDefaultParagraphSeparator() == ParagraphSeparator::br ||
      !HTMLEditUtils::ShouldInsertLinefeedCharacter(atStartOfSelection,
                                                    *editingHost)) {
    CreateElementResult insertBRElementResult = InsertBRElement(
        WithTransaction::Yes, atStartOfSelection, nsIEditor::eNext);
    if (insertBRElementResult.isErr()) {
      NS_WARNING("HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
      return insertBRElementResult.unwrapErr();
    }
    nsresult rv = insertBRElementResult.SuggestCaretPointTo(*this, {});
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "CreateElementResult::SuggestCaretPointTo() failed");
    MOZ_ASSERT(insertBRElementResult.GetNewNode());
    return rv;
  }

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (NS_SUCCEEDED(rv) && SelectionRef().IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterInvisibleBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterInvisibleBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  firstRange = SelectionRef().GetRangeAt(0);
  if (NS_WARN_IF(!firstRange)) {
    return NS_ERROR_FAILURE;
  }

  atStartOfSelection = EditorDOMPoint(firstRange->StartRef());
  if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
    return NS_ERROR_FAILURE;
  }
  MOZ_ASSERT(atStartOfSelection.IsSetAndValid());

  // Do nothing if the node is read-only
  if (!HTMLEditUtils::IsSimplyEditableNode(
          *atStartOfSelection.GetContainer())) {
    return NS_SUCCESS_DOM_NO_OPERATION;
  }

  Result<EditorDOMPoint, nsresult> insertLineFeedResult =
      HandleInsertLinefeed(atStartOfSelection, *editingHost);
  if (MOZ_UNLIKELY(insertLineFeedResult.isErr())) {
    NS_WARNING("HTMLEditor::HandleInsertLinefeed() failed");
    return insertLineFeedResult.unwrapErr();
  }
  rv = CollapseSelectionTo(insertLineFeedResult.inspect());
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::CollapseSelectionTo() failed");
  return rv;
}

EditActionResult HTMLEditor::InsertParagraphSeparatorAsSubAction(
    const Element& aEditingHost) {
  if (NS_WARN_IF(!mInitSucceeded)) {
    return EditActionIgnored(NS_ERROR_NOT_INITIALIZED);
  }

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  // XXX This may be called by execCommand() with "insertParagraph".
  //     In such case, naming the transaction "TypingTxnName" is odd.
  AutoPlaceholderBatch treatAsOneTransaction(*this, *nsGkAtoms::TypingTxnName,
                                             ScrollSelectionIntoView::Yes,
                                             __FUNCTION__);

  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this, EditSubAction::eInsertParagraphSeparator, nsIEditor::eNext,
      ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return EditActionResult(ignoredError.StealNSResult());
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  UndefineCaretBidiLevel();

  // If the selection isn't collapsed, delete it.
  if (!SelectionRef().IsCollapsed()) {
    nsresult rv =
        DeleteSelectionAsSubAction(nsIEditor::eNone, nsIEditor::eStrip);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "EditorBase::DeleteSelectionAsSubAction(eNone, eStrip) failed");
      return EditActionIgnored(rv);
    }
  }

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionIgnored(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (NS_SUCCEEDED(rv) && SelectionRef().IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterInvisibleBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionIgnored(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterInvisibleBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionIgnored(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  AutoRangeArray selectionRanges(SelectionRef());
  {
    // If the editing host is the body element, the selection may be outside
    // aEditingHost.  In the case, we should use the editing host outside the
    // <body> only here for keeping our traditional behavior for now.
    // This should be fixed in bug 1634351.
    const Element* editingHostMaybeOutsideBody = &aEditingHost;
    if (aEditingHost.IsHTMLElement(nsGkAtoms::body)) {
      editingHostMaybeOutsideBody = ComputeEditingHost(LimitInBodyElement::No);
      if (NS_WARN_IF(!editingHostMaybeOutsideBody)) {
        return EditActionIgnored(NS_ERROR_FAILURE);
      }
    }
    selectionRanges.EnsureOnlyEditableRanges(*editingHostMaybeOutsideBody);
    if (NS_WARN_IF(selectionRanges.Ranges().IsEmpty())) {
      return EditActionIgnored(NS_ERROR_FAILURE);
    }
  }

  auto pointToInsert =
      selectionRanges.GetFirstRangeStartPoint<EditorDOMPoint>();
  if (NS_WARN_IF(!pointToInsert.IsInContentNode())) {
    return EditActionIgnored(NS_ERROR_FAILURE);
  }

  if (IsMailEditor()) {
    if (RefPtr<Element> mailCiteElement = GetMostDistantAncestorMailCiteElement(
            *pointToInsert.ContainerAs<nsIContent>())) {
      // Split any mailcites in the way.  Should we abort this if we encounter
      // table cell boundaries?
      Result<EditorDOMPoint, nsresult> atNewBRElementOrError =
          HandleInsertParagraphInMailCiteElement(*mailCiteElement,
                                                 pointToInsert, aEditingHost);
      if (MOZ_UNLIKELY(atNewBRElementOrError.isErr())) {
        NS_WARNING(
            "HTMLEditor::HandleInsertParagraphInMailCiteElement() failed");
        return EditActionHandled(atNewBRElementOrError.unwrapErr());
      }
      EditorDOMPoint pointToPutCaret = atNewBRElementOrError.unwrap();
      MOZ_ASSERT(pointToPutCaret.IsSet());
      pointToPutCaret.SetInterlinePosition(InterlinePosition::StartOfNextLine);
      MOZ_ASSERT(pointToPutCaret.GetChild());
      MOZ_ASSERT(pointToPutCaret.GetChild()->IsHTMLElement(nsGkAtoms::br));
      nsresult rv = CollapseSelectionTo(pointToPutCaret);
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "EditorBase::CollapseSelectionTo() failed");
      return EditActionHandled(rv);
    }
  }

  // If the active editing host is an inline element, or if the active editing
  // host is the block parent itself and we're configured to use <br> as a
  // paragraph separator, just append a <br>.
  // If the editing host parent element is editable, it means that the editing
  // host must be a <body> element and the selection may be outside the body
  // element.  If the selection is outside the editing host, we should not
  // insert new paragraph nor <br> element.
  // XXX Currently, we don't support editing outside <body> element, but Blink
  //     does it.
  if (aEditingHost.GetParentElement() &&
      HTMLEditUtils::IsSimplyEditableNode(*aEditingHost.GetParentElement()) &&
      !nsContentUtils::ContentIsFlattenedTreeDescendantOf(
          pointToInsert.ContainerAs<nsIContent>(), &aEditingHost)) {
    return EditActionHandled(NS_ERROR_EDITOR_NO_EDITABLE_RANGE);
  }

  auto InsertLineBreakInstead =
      [](const Element* aEditableBlockElement,
         const EditorDOMPoint& aCandidatePointToSplit,
         ParagraphSeparator aDefaultParagraphSeparator,
         const Element& aEditingHost) {
        // If there is no block parent in the editing host, i.e., the editing
        // host itself is also a non-block element, we should insert a line
        // break.
        if (!aEditableBlockElement) {
          // XXX Chromium checks if the CSS box of the editing host is a block.
          return true;
        }

        // If the editable block element is not splittable, e.g., it's an
        // editing host, and the default paragraph separator is <br> or the
        // element cannot contain a <p> element, we should insert a <br>
        // element.
        if (!HTMLEditUtils::IsSplittableNode(*aEditableBlockElement)) {
          return aDefaultParagraphSeparator == ParagraphSeparator::br ||
                 !HTMLEditUtils::CanElementContainParagraph(aEditingHost) ||
                 HTMLEditUtils::ShouldInsertLinefeedCharacter(
                     aCandidatePointToSplit, aEditingHost);
        }

        // If the nearest block parent is a single-line container declared in
        // the execCommand spec and not the editing host, we should separate the
        // block even if the default paragraph separator is <br> element.
        if (HTMLEditUtils::IsSingleLineContainer(*aEditableBlockElement)) {
          return false;
        }

        // Otherwise, unless there is no block ancestor which can contain <p>
        // element, we shouldn't insert a line break here.
        for (const Element* editableBlockAncestor = aEditableBlockElement;
             editableBlockAncestor;
             editableBlockAncestor = HTMLEditUtils::GetAncestorElement(
                 *editableBlockAncestor,
                 HTMLEditUtils::ClosestEditableBlockElement)) {
          if (HTMLEditUtils::CanElementContainParagraph(
                  *editableBlockAncestor)) {
            return false;
          }
        }
        return true;
      };

  // Look for the nearest parent block.  However, don't return error even if
  // there is no block parent here because in such case, i.e., editing host
  // is an inline element, we should insert <br> simply.
  RefPtr<Element> editableBlockElement =
      HTMLEditUtils::GetInclusiveAncestorElement(
          *pointToInsert.ContainerAs<nsIContent>(),
          HTMLEditUtils::ClosestEditableBlockElement);

  // If we cannot insert a <p>/<div> element at the selection, we should insert
  // a <br> element or a linefeed instead.
  const ParagraphSeparator separator = GetDefaultParagraphSeparator();
  if (InsertLineBreakInstead(editableBlockElement, pointToInsert, separator,
                             aEditingHost)) {
    // For backward compatibility, we should not insert a linefeed if
    // paragraph separator is set to "br" which is Gecko-specific mode.
    if (separator != ParagraphSeparator::br &&
        HTMLEditUtils::ShouldInsertLinefeedCharacter(pointToInsert,
                                                     aEditingHost)) {
      Result<EditorDOMPoint, nsresult> insertLineFeedResult =
          HandleInsertLinefeed(pointToInsert, aEditingHost);
      if (MOZ_UNLIKELY(insertLineFeedResult.isErr())) {
        NS_WARNING("HTMLEditor::HandleInsertLinefeed() failed");
        return EditActionResult(insertLineFeedResult.unwrapErr());
      }
      nsresult rv = CollapseSelectionTo(insertLineFeedResult.inspect());
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::CollapseSelectionTo() failed");
        return EditActionResult(rv);
      }
      return EditActionHandled();
    }

    CreateElementResult insertBRElementResult =
        HandleInsertBRElement(pointToInsert, aEditingHost);
    if (insertBRElementResult.isErr()) {
      NS_WARNING("HTMLEditor::HandleInsertBRElement() failed");
      return EditActionHandled(insertBRElementResult.unwrapErr());
    }
    nsresult rv = insertBRElementResult.SuggestCaretPointTo(*this, {});
    if (NS_FAILED(rv)) {
      NS_WARNING("CreateElementResult::SuggestCaretPointTo() failed");
      return EditActionHandled(rv);
    }
    return EditActionHandled();
  }

  // If somebody wants to restrict caret position in a block element below,
  // we should guarantee it.  Otherwise, we can put caret to the candidate
  // point.
  auto CollapseSelection =
      [this](const EditorDOMPoint& aCandidatePointToPutCaret,
             const Element* aBlockElementShouldHaveCaret,
             const SuggestCaretOptions& aOptions)
          MOZ_CAN_RUN_SCRIPT -> nsresult {
    if (!aCandidatePointToPutCaret.IsSet()) {
      if (aOptions.contains(SuggestCaret::OnlyIfHasSuggestion)) {
        return NS_OK;
      }
      return aOptions.contains(SuggestCaret::AndIgnoreTrivialError)
                 ? NS_SUCCESS_EDITOR_BUT_IGNORED_TRIVIAL_ERROR
                 : NS_ERROR_FAILURE;
    }
    EditorDOMPoint pointToPutCaret(aCandidatePointToPutCaret);
    if (aBlockElementShouldHaveCaret) {
      Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
          HTMLEditUtils::ComputePointToPutCaretInElementIfOutside<
              EditorDOMPoint>(*aBlockElementShouldHaveCaret,
                              aCandidatePointToPutCaret);
      if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
        NS_WARNING(
            "HTMLEditUtils::ComputePointToPutCaretInElementIfOutside() "
            "failed, but ignored");
      } else if (pointToPutCaretOrError.inspect().IsSet()) {
        pointToPutCaret = pointToPutCaretOrError.unwrap();
      }
    }
    nsresult rv = CollapseSelectionTo(pointToPutCaret);
    if (NS_FAILED(rv) && MOZ_LIKELY(rv != NS_ERROR_EDITOR_DESTROYED) &&
        aOptions.contains(SuggestCaret::AndIgnoreTrivialError)) {
      rv = NS_SUCCESS_EDITOR_BUT_IGNORED_TRIVIAL_ERROR;
    }
    return rv;
  };

  RefPtr<Element> blockElementToPutCaret;
  if (!HTMLEditUtils::IsSplittableNode(*editableBlockElement) &&
      separator != ParagraphSeparator::br) {
    // Insert a new block first
    MOZ_ASSERT(separator == ParagraphSeparator::div ||
               separator == ParagraphSeparator::p);
    Result<RefPtr<Element>, nsresult> suggestBlockElementToPutCaretOrError =
        FormatBlockContainerWithTransaction(
            selectionRanges,
            MOZ_KnownLive(HTMLEditor::ToParagraphSeparatorTagName(separator)),
            aEditingHost);
    if (MOZ_UNLIKELY(suggestBlockElementToPutCaretOrError.isErr())) {
      NS_WARNING("HTMLEditor::FormatBlockContainerWithTransaction() failed");
      return EditActionResult(suggestBlockElementToPutCaretOrError.unwrapErr());
    }
    if (selectionRanges.HasSavedRanges()) {
      selectionRanges.RestoreFromSavedRanges();
    }
    pointToInsert = selectionRanges.GetFirstRangeStartPoint<EditorDOMPoint>();
    if (NS_WARN_IF(!pointToInsert.IsInContentNode())) {
      return EditActionIgnored(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
    }
    MOZ_ASSERT(pointToInsert.IsSetAndValid());
    blockElementToPutCaret = suggestBlockElementToPutCaretOrError.unwrap();

    editableBlockElement = HTMLEditUtils::GetInclusiveAncestorElement(
        *pointToInsert.ContainerAs<nsIContent>(),
        HTMLEditUtils::ClosestEditableBlockElement);
    if (NS_WARN_IF(!editableBlockElement)) {
      return EditActionIgnored(NS_ERROR_UNEXPECTED);
    }
    if (NS_WARN_IF(!HTMLEditUtils::IsSplittableNode(*editableBlockElement))) {
      // Didn't create a new block for some reason, fall back to <br>
      CreateElementResult insertBRElementResult =
          HandleInsertBRElement(pointToInsert, aEditingHost);
      if (insertBRElementResult.isErr()) {
        NS_WARNING("HTMLEditor::HandleInsertBRElement() failed");
        return EditActionResult(insertBRElementResult.unwrapErr());
      }
      EditorDOMPoint pointToPutCaret = insertBRElementResult.UnwrapCaretPoint();
      if (MOZ_UNLIKELY(!pointToPutCaret.IsSet())) {
        NS_WARNING(
            "HTMLEditor::HandleInsertBRElement() didn't suggest a point to put "
            "caret");
        return EditActionHandled(NS_ERROR_FAILURE);
      }
      nsresult rv =
          CollapseSelection(pointToPutCaret, blockElementToPutCaret, {});
      if (NS_FAILED(rv)) {
        NS_WARNING("CollapseSelection() failed");
        return EditActionHandled(rv);
      }
      return EditActionHandled();
    }
    // We want to collapse selection in the editable block element.
    blockElementToPutCaret = editableBlockElement;
  }

  // If block is empty, populate with br.  (For example, imagine a div that
  // contains the word "text".  The user selects "text" and types return.
  // "Text" is deleted leaving an empty block.  We want to put in one br to
  // make block have a line.  Then code further below will put in a second br.)
  RefPtr<Element> insertedPaddingBRElement;
  if (HTMLEditUtils::IsEmptyBlockElement(
          *editableBlockElement,
          {EmptyCheckOption::TreatSingleBRElementAsVisible})) {
    CreateElementResult insertBRElementResult = InsertBRElement(
        WithTransaction::Yes, EditorDOMPoint::AtEndOf(*editableBlockElement));
    if (insertBRElementResult.isErr()) {
      NS_WARNING("HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
      return EditActionIgnored(insertBRElementResult.unwrapErr());
    }
    insertBRElementResult.IgnoreCaretPointSuggestion();
    MOZ_ASSERT(insertBRElementResult.GetNewNode());
    insertedPaddingBRElement = insertBRElementResult.UnwrapNewNode();

    pointToInsert = selectionRanges.GetFirstRangeStartPoint<EditorDOMPoint>();
    if (NS_WARN_IF(!pointToInsert.IsInContentNode())) {
      return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
    }
  }

  RefPtr<Element> maybeNonEditableListItem =
      HTMLEditUtils::GetClosestAncestorListItemElement(*editableBlockElement,
                                                       &aEditingHost);
  if (maybeNonEditableListItem &&
      HTMLEditUtils::IsSplittableNode(*maybeNonEditableListItem)) {
    Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
        HandleInsertParagraphInListItemElement(*maybeNonEditableListItem,
                                               pointToInsert, aEditingHost);
    if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
      if (NS_WARN_IF(pointToPutCaretOrError.unwrapErr() ==
                     NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING(
          "HTMLEditor::HandleInsertParagraphInListItemElement() failed, but "
          "ignored");
      return EditActionHandled();
    }
    nsresult rv = CollapseSelection(pointToPutCaretOrError.inspect(),
                                    blockElementToPutCaret,
                                    {SuggestCaret::AndIgnoreTrivialError});
    if (NS_FAILED(rv)) {
      NS_WARNING("CollapseSelection() failed");
      return EditActionHandled(rv);
    }
    NS_WARNING_ASSERTION(rv != NS_SUCCESS_EDITOR_BUT_IGNORED_TRIVIAL_ERROR,
                         "CollapseSelection() failed, but ignored");
    return EditActionHandled();
  }

  if (HTMLEditUtils::IsHeader(*editableBlockElement)) {
    SplitNodeResult splitHeadingElementResult =
        HandleInsertParagraphInHeadingElement(*editableBlockElement,
                                              pointToInsert);
    if (MOZ_UNLIKELY(splitHeadingElementResult.isErr())) {
      NS_WARNING("HTMLEditor::HandleInsertParagraphInHeadingElement() failed");
      return EditActionHandled();
    }
    EditorDOMPoint pointToPutCaret =
        splitHeadingElementResult.UnwrapCaretPoint();
    nsresult rv = CollapseSelection(pointToPutCaret, blockElementToPutCaret,
                                    {SuggestCaret::OnlyIfHasSuggestion,
                                     SuggestCaret::AndIgnoreTrivialError});
    if (NS_FAILED(rv)) {
      NS_WARNING("CollapseSelection() failed");
      return EditActionHandled(rv);
    }
    NS_WARNING_ASSERTION(rv != NS_SUCCESS_EDITOR_BUT_IGNORED_TRIVIAL_ERROR,
                         "CollapseSelection() failed, but ignored");
    return EditActionHandled();
  }

  // XXX Ideally, we should take same behavior with both <p> container and
  //     <div> container.  However, we are still using <br> as default
  //     paragraph separator (non-standard) and we've split only <p> container
  //     long time.  Therefore, some web apps may depend on this behavior like
  //     Gmail.  So, let's use traditional odd behavior only when the default
  //     paragraph separator is <br>.  Otherwise, take consistent behavior
  //     between <p> container and <div> container.
  if ((separator == ParagraphSeparator::br &&
       editableBlockElement->IsHTMLElement(nsGkAtoms::p)) ||
      (separator != ParagraphSeparator::br &&
       editableBlockElement->IsAnyOfHTMLElements(nsGkAtoms::p,
                                                 nsGkAtoms::div))) {
    // Paragraphs: special rules to look for <br>s
    SplitNodeResult splitResult = HandleInsertParagraphInParagraph(
        *editableBlockElement,
        insertedPaddingBRElement ? EditorDOMPoint(insertedPaddingBRElement)
                                 : pointToInsert,
        aEditingHost);
    if (splitResult.isErr()) {
      NS_WARNING("HTMLEditor::HandleInsertParagraphInParagraph() failed");
      return EditActionResult(splitResult.unwrapErr());
    }
    if (splitResult.Handled()) {
      EditorDOMPoint pointToPutCaret = splitResult.UnwrapCaretPoint();
      nsresult rv = CollapseSelection(pointToPutCaret, blockElementToPutCaret,
                                      {SuggestCaret::AndIgnoreTrivialError});
      if (NS_FAILED(rv)) {
        NS_WARNING("CollapseSelection() failed");
        return EditActionResult(rv);
      }
      NS_WARNING_ASSERTION(rv != NS_SUCCESS_EDITOR_BUT_IGNORED_TRIVIAL_ERROR,
                           "CollapseSelection() failed, but ignored");
      return EditActionHandled();
    }
    MOZ_ASSERT(!splitResult.HasCaretPointSuggestion());

    // Fall through, if HandleInsertParagraphInParagraph() didn't handle it.
    MOZ_ASSERT(!result.Canceled());
    MOZ_ASSERT(result.Rv() == NS_SUCCESS_DOM_NO_OPERATION);
    MOZ_ASSERT(pointToInsert.IsSetAndValid(),
               "HTMLEditor::HandleInsertParagraphInParagraph() shouldn't touch "
               "the DOM tree if it returns not-handled state");
  }

  // If nobody handles this edit action, let's insert new <br> at the selection.
  CreateElementResult insertBRElementResult =
      HandleInsertBRElement(pointToInsert, aEditingHost);
  if (insertBRElementResult.isErr()) {
    NS_WARNING("HTMLEditor::HandleInsertBRElement() failed");
    return EditActionIgnored(insertBRElementResult.unwrapErr());
  }
  EditorDOMPoint pointToPutCaret = insertBRElementResult.UnwrapCaretPoint();
  rv = CollapseSelection(pointToPutCaret, blockElementToPutCaret, {});
  if (NS_FAILED(rv)) {
    NS_WARNING("CreateElementResult::SuggestCaretPointTo() failed");
    return EditActionHandled(rv);
  }
  return EditActionHandled();
}

CreateElementResult HTMLEditor::HandleInsertBRElement(
    const EditorDOMPoint& aPointToBreak, const Element& aEditingHost) {
  MOZ_ASSERT(aPointToBreak.IsSet());
  MOZ_ASSERT(IsEditActionDataAvailable());

  bool brElementIsAfterBlock = false, brElementIsBeforeBlock = false;

  // First, insert a <br> element.
  RefPtr<Element> brElement;
  if (IsInPlaintextMode()) {
    CreateElementResult insertBRElementResult =
        InsertBRElement(WithTransaction::Yes, aPointToBreak);
    if (insertBRElementResult.isErr()) {
      NS_WARNING("HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
      return CreateElementResult(insertBRElementResult.unwrapErr());
    }
    // We'll return with suggesting new caret position and nobody refers
    // selection after here.  So we don't need to update selection here.
    insertBRElementResult.IgnoreCaretPointSuggestion();
    MOZ_ASSERT(insertBRElementResult.GetNewNode());
    brElement = insertBRElementResult.UnwrapNewNode();
  } else {
    EditorDOMPoint pointToBreak(aPointToBreak);
    WSRunScanner wsRunScanner(&aEditingHost, pointToBreak);
    WSScanResult backwardScanResult =
        wsRunScanner.ScanPreviousVisibleNodeOrBlockBoundaryFrom(pointToBreak);
    if (MOZ_UNLIKELY(backwardScanResult.Failed())) {
      NS_WARNING(
          "WSRunScanner::ScanPreviousVisibleNodeOrBlockBoundaryFrom() failed");
      return CreateElementResult(NS_ERROR_FAILURE);
    }
    brElementIsAfterBlock = backwardScanResult.ReachedBlockBoundary();
    WSScanResult forwardScanResult =
        wsRunScanner.ScanNextVisibleNodeOrBlockBoundaryFrom(pointToBreak);
    if (MOZ_UNLIKELY(forwardScanResult.Failed())) {
      NS_WARNING(
          "WSRunScanner::ScanNextVisibleNodeOrBlockBoundaryFrom() failed");
      return CreateElementResult(NS_ERROR_FAILURE);
    }
    brElementIsBeforeBlock = forwardScanResult.ReachedBlockBoundary();
    // If the container of the break is a link, we need to split it and
    // insert new <br> between the split links.
    RefPtr<Element> linkNode =
        HTMLEditor::GetLinkElement(pointToBreak.GetContainer());
    if (linkNode) {
      const SplitNodeResult splitLinkNodeResult = SplitNodeDeepWithTransaction(
          *linkNode, pointToBreak, SplitAtEdges::eDoNotCreateEmptyContainer);
      if (splitLinkNodeResult.isErr()) {
        NS_WARNING(
            "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
            "eDoNotCreateEmptyContainer) failed");
        return CreateElementResult(splitLinkNodeResult.unwrapErr());
      }
      // TODO: Some methods called by
      //       WhiteSpaceVisibilityKeeper::InsertBRElement() use
      //       ComputeEditingHost() which depends on selection.  Therefore,
      //       we cannot skip updating selection here.
      nsresult rv = splitLinkNodeResult.SuggestCaretPointTo(
          *this, {SuggestCaret::OnlyIfHasSuggestion,
                  SuggestCaret::OnlyIfTransactionsAllowedToDoIt});
      if (NS_FAILED(rv)) {
        NS_WARNING("SplitNodeResult::SuggestCaretPointTo() failed");
        return CreateElementResult(rv);
      }
      pointToBreak = splitLinkNodeResult.AtSplitPoint<EditorDOMPoint>();
      // When adding caret suggestion to SplitNodeResult, here didn't change
      // selection so that just ignore it.
      splitLinkNodeResult.IgnoreCaretPointSuggestion();
    }
    CreateElementResult insertBRElementResult =
        WhiteSpaceVisibilityKeeper::InsertBRElement(*this, pointToBreak,
                                                    aEditingHost);
    if (insertBRElementResult.isErr()) {
      NS_WARNING("WhiteSpaceVisibilityKeeper::InsertBRElement() failed");
      return CreateElementResult(insertBRElementResult.unwrapErr());
    }
    // We'll return with suggesting new caret position and nobody refers
    // selection after here.  So we don't need to update selection here.
    insertBRElementResult.IgnoreCaretPointSuggestion();
    brElement = insertBRElementResult.UnwrapNewNode();
    MOZ_ASSERT(brElement);
  }

  if (MOZ_UNLIKELY(!brElement->GetParentNode())) {
    NS_WARNING("Inserted <br> element was removed by the web app");
    return CreateElementResult(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  if (brElementIsAfterBlock && brElementIsBeforeBlock) {
    // We just placed a <br> between block boundaries.  This is the one case
    // where we want the selection to be before the br we just placed, as the
    // br will be on a new line, rather than at end of prior line.
    // XXX brElementIsAfterBlock and brElementIsBeforeBlock were set before
    //     modifying the DOM tree.  So, now, the <br> element may not be
    //     between blocks.
    EditorDOMPoint pointToPutCaret(brElement,
                                   InterlinePosition::StartOfNextLine);
    return CreateElementResult(std::move(brElement),
                               std::move(pointToPutCaret));
  }

  auto afterBRElement = EditorDOMPoint::After(brElement);
  WSScanResult forwardScanFromAfterBRElementResult =
      WSRunScanner::ScanNextVisibleNodeOrBlockBoundary(&aEditingHost,
                                                       afterBRElement);
  if (MOZ_UNLIKELY(forwardScanFromAfterBRElementResult.Failed())) {
    NS_WARNING("WSRunScanner::ScanNextVisibleNodeOrBlockBoundary() failed");
    return CreateElementResult(NS_ERROR_FAILURE);
  }
  if (forwardScanFromAfterBRElementResult.ReachedBRElement()) {
    // The next thing after the break we inserted is another break.  Move the
    // second break to be the first break's sibling.  This will prevent them
    // from being in different inline nodes, which would break
    // SetInterlinePosition().  It will also assure that if the user clicks
    // away and then clicks back on their new blank line, they will still get
    // the style from the line above.
    if (brElement->GetNextSibling() !=
        forwardScanFromAfterBRElementResult.BRElementPtr()) {
      MOZ_ASSERT(forwardScanFromAfterBRElementResult.BRElementPtr());
      const MoveNodeResult moveBRElementResult = MoveNodeWithTransaction(
          MOZ_KnownLive(*forwardScanFromAfterBRElementResult.BRElementPtr()),
          afterBRElement);
      if (moveBRElementResult.isErr()) {
        NS_WARNING("HTMLEditor::MoveNodeWithTransaction() failed");
        return CreateElementResult(moveBRElementResult.unwrapErr());
      }
      nsresult rv = moveBRElementResult.SuggestCaretPointTo(
          *this, {SuggestCaret::OnlyIfHasSuggestion,
                  SuggestCaret::OnlyIfTransactionsAllowedToDoIt,
                  SuggestCaret::AndIgnoreTrivialError});
      if (NS_FAILED(rv)) {
        NS_WARNING("MoveNodeResult::SuggestCaretPointTo() failed");
        return CreateElementResult(rv);
      }
      NS_WARNING_ASSERTION(
          rv != NS_SUCCESS_EDITOR_BUT_IGNORED_TRIVIAL_ERROR,
          "MoveNodeResult::SuggestCaretPointTo() failed, but ignored");
    }
  }

  // We want the caret to stick to whatever is past the break.  This is because
  // the break is on the same line we were on, but the next content will be on
  // the following line.

  // An exception to this is if the break has a next sibling that is a block
  // node.  Then we stick to the left to avoid an uber caret.
  nsIContent* nextSiblingOfBRElement = brElement->GetNextSibling();
  afterBRElement.SetInterlinePosition(
      nextSiblingOfBRElement &&
              HTMLEditUtils::IsBlockElement(*nextSiblingOfBRElement)
          ? InterlinePosition::EndOfLine
          : InterlinePosition::StartOfNextLine);
  return CreateElementResult(std::move(brElement), afterBRElement);
}

Result<EditorDOMPoint, nsresult> HTMLEditor::HandleInsertLinefeed(
    const EditorDOMPoint& aPointToBreak, const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  if (NS_WARN_IF(!aPointToBreak.IsSet())) {
    return Err(NS_ERROR_INVALID_ARG);
  }

  const RefPtr<Document> document = GetDocument();
  MOZ_DIAGNOSTIC_ASSERT(document);
  if (NS_WARN_IF(!document)) {
    return Err(NS_ERROR_FAILURE);
  }

  // TODO: The following code is duplicated from `HandleInsertText`.  They
  //       should be merged when we fix bug 92921.

  Result<EditorDOMPoint, nsresult> setStyleResult =
      CreateStyleForInsertText(aPointToBreak);
  if (MOZ_UNLIKELY(setStyleResult.isErr())) {
    NS_WARNING("HTMLEditor::CreateStyleForInsertText() failed");
    return setStyleResult.propagateErr();
  }

  EditorDOMPoint pointToInsert = setStyleResult.inspect().IsSet()
                                     ? setStyleResult.inspect()
                                     : aPointToBreak;
  if (NS_WARN_IF(!pointToInsert.IsSetAndValid()) ||
      NS_WARN_IF(!pointToInsert.IsInContentNode())) {
    return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }
  MOZ_ASSERT(pointToInsert.IsSetAndValid());

  // The node may not be able to have a text node so that we need to check it
  // here.
  if (!pointToInsert.IsInTextNode() &&
      !HTMLEditUtils::CanNodeContain(*pointToInsert.ContainerAs<nsIContent>(),
                                     *nsGkAtoms::textTagName)) {
    NS_WARNING(
        "HTMLEditor::HandleInsertLinefeed() couldn't insert a linefeed because "
        "the insertion position couldn't have text nodes");
    return Err(NS_ERROR_EDITOR_NO_EDITABLE_RANGE);
  }

  AutoRestore<bool> disableListener(
      EditSubActionDataRef().mAdjustChangedRangeFromListener);
  EditSubActionDataRef().mAdjustChangedRangeFromListener = false;

  // TODO: We don't need AutoTransactionsConserveSelection here in the normal
  //       cases, but removing this may cause the behavior with the legacy
  //       mutation event listeners.  We should try to delete this in a bug.
  AutoTransactionsConserveSelection dontChangeMySelection(*this);

  EditorDOMPoint pointToPutCaret;
  {
    AutoTrackDOMPoint trackingInsertingPosition(RangeUpdaterRef(),
                                                &pointToInsert);
    Result<EditorDOMPoint, nsresult> insertTextResult =
        InsertTextWithTransaction(*document, u"\n"_ns, pointToInsert);
    if (MOZ_UNLIKELY(insertTextResult.isErr())) {
      NS_WARNING("HTMLEditor::InsertTextWithTransaction() failed");
      return insertTextResult;
    }
    pointToPutCaret = insertTextResult.unwrap();
  }

  // Insert a padding <br> element at the end of the block element if there is
  // no content between the inserted linefeed and the following block boundary
  // to make sure that the last line is visible.
  // XXX Blink/WebKit inserts another linefeed character in this case.  However,
  //     for doing it, we need more work, e.g., updating serializer, deleting
  //     unnecessary padding <br> element at modifying the last line.
  if (pointToPutCaret.IsInContentNode() && pointToPutCaret.IsEndOfContainer()) {
    WSRunScanner wsScannerAtCaret(&aEditingHost, pointToPutCaret);
    if (wsScannerAtCaret.StartsFromPreformattedLineBreak() &&
        wsScannerAtCaret.EndsByBlockBoundary() &&
        HTMLEditUtils::CanNodeContain(*wsScannerAtCaret.GetEndReasonContent(),
                                      *nsGkAtoms::br)) {
      AutoTrackDOMPoint trackingInsertedPosition(RangeUpdaterRef(),
                                                 &pointToInsert);
      AutoTrackDOMPoint trackingNewCaretPosition(RangeUpdaterRef(),
                                                 &pointToPutCaret);
      CreateElementResult insertBRElementResult =
          InsertBRElement(WithTransaction::Yes, pointToPutCaret);
      if (insertBRElementResult.isErr()) {
        NS_WARNING("HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
        return Err(insertBRElementResult.unwrapErr());
      }
      // We're tracking next caret position with newCaretPosition.  Therefore,
      // we don't need to update selection here.
      insertBRElementResult.IgnoreCaretPointSuggestion();
      MOZ_ASSERT(insertBRElementResult.GetNewNode());
    }
  }

  // manually update the doc changed range so that
  // OnEndHandlingTopLevelEditSubActionInternal will clean up the correct
  // portion of the document.
  MOZ_ASSERT(pointToPutCaret.IsSet());
  if (NS_WARN_IF(!pointToPutCaret.IsSet())) {
    // XXX Here is odd.  We did mChangedRange->SetStartAndEnd(pointToInsert,
    //     pointToPutCaret), but it always fails because of the latter is unset.
    //     Therefore, always returning NS_ERROR_FAILURE from here is the
    //     traditional behavior...
    // TODO: Stop updating the interline position of Selection with fixing here
    //       and returning expected point.
    DebugOnly<nsresult> rvIgnored =
        SelectionRef().SetInterlinePosition(InterlinePosition::EndOfLine);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                         "Selection::SetInterlinePosition(InterlinePosition::"
                         "EndOfLine) failed, but ignored");
    if (NS_FAILED(TopLevelEditSubActionDataRef().mChangedRange->CollapseTo(
            pointToInsert))) {
      NS_WARNING("nsRange::CollapseTo() failed");
      return Err(NS_ERROR_FAILURE);
    }
    NS_WARNING(
        "We always return NS_ERROR_FAILURE here because of a failure of "
        "updating mChangedRange");
    return Err(NS_ERROR_FAILURE);
  }

  if (NS_FAILED(TopLevelEditSubActionDataRef().mChangedRange->SetStartAndEnd(
          pointToInsert.ToRawRangeBoundary(),
          pointToPutCaret.ToRawRangeBoundary()))) {
    NS_WARNING("nsRange::SetStartAndEnd() failed");
    return Err(NS_ERROR_FAILURE);
  }

  pointToPutCaret.SetInterlinePosition(InterlinePosition::EndOfLine);
  return pointToPutCaret;
}

Result<EditorDOMPoint, nsresult>
HTMLEditor::HandleInsertParagraphInMailCiteElement(
    Element& aMailCiteElement, const EditorDOMPoint& aPointToSplit,
    const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aPointToSplit.IsSet());
  NS_ASSERTION(!HTMLEditUtils::IsEmptyNode(aMailCiteElement),
               "The mail-cite element will be deleted, does it expected result "
               "for you?");

  const SplitNodeResult splitCiteElementResult = [&]() MOZ_CAN_RUN_SCRIPT {
    EditorDOMPoint pointToSplit(aPointToSplit);

    // If our selection is just before a break, nudge it to be just after
    // it. This does two things for us.  It saves us the trouble of having
    // to add a break here ourselves to preserve the "blockness" of the
    // inline span mailquote (in the inline case), and : it means the break
    // won't end up making an empty line that happens to be inside a
    // mailquote (in either inline or block case). The latter can confuse a
    // user if they click there and start typing, because being in the
    // mailquote may affect wrapping behavior, or font color, etc.
    WSScanResult forwardScanFromPointToSplitResult =
        WSRunScanner::ScanNextVisibleNodeOrBlockBoundary(&aEditingHost,
                                                         pointToSplit);
    if (forwardScanFromPointToSplitResult.Failed()) {
      return SplitNodeResult(NS_ERROR_FAILURE);
    }
    // If selection start point is before a break and it's inside the
    // mailquote, let's split it after the visible node.
    if (forwardScanFromPointToSplitResult.ReachedBRElement() &&
        forwardScanFromPointToSplitResult.BRElementPtr() != &aMailCiteElement &&
        aMailCiteElement.Contains(
            forwardScanFromPointToSplitResult.BRElementPtr())) {
      pointToSplit =
          forwardScanFromPointToSplitResult.PointAfterContent<EditorDOMPoint>();
    }

    if (NS_WARN_IF(!pointToSplit.IsInContentNode())) {
      return SplitNodeResult(NS_ERROR_FAILURE);
    }

    SplitNodeResult splitResult =
        SplitNodeDeepWithTransaction(aMailCiteElement, pointToSplit,
                                     SplitAtEdges::eDoNotCreateEmptyContainer);
    if (splitResult.isErr()) {
      NS_WARNING(
          "HTMLEditor::SplitNodeDeepWithTransaction(aMailCiteElement, "
          "SplitAtEdges::eDoNotCreateEmptyContainer) failed");
      return splitResult;
    }
    nsresult rv = splitResult.SuggestCaretPointTo(
        *this, {SuggestCaret::OnlyIfHasSuggestion,
                SuggestCaret::OnlyIfTransactionsAllowedToDoIt});
    if (NS_FAILED(rv)) {
      NS_WARNING("SplitNodeResult::SuggestCaretPointTo() failed");
      return SplitNodeResult(rv);
    }
    return splitResult;
  }();
  if (splitCiteElementResult.isErr()) {
    NS_WARNING("Failed to split a mail-cite element");
    return Err(splitCiteElementResult.unwrapErr());
  }
  // When adding caret suggestion to SplitNodeResult, here didn't change
  // selection so that just ignore it.
  splitCiteElementResult.IgnoreCaretPointSuggestion();

  // Add an invisible <br> to the end of left cite node if it was a <span> of
  // style="display: block".  This is important, since when serializing the cite
  // to plain text, the span which caused the visual break is discarded.  So the
  // added <br> will guarantee that the serializer will insert a break where the
  // user saw one.
  // FYI: splitCiteElementResult grabs the previous node and the next node with
  //      nsCOMPtr or EditorDOMPoint.  So, it's safe to access leftCiteElement
  //      and rightCiteElement even after changing the DOM tree and/or selection
  //      even though it's raw pointer.
  Element* const leftCiteElement =
      Element::FromNodeOrNull(splitCiteElementResult.GetPreviousContent());
  Element* const rightCiteElement =
      Element::FromNodeOrNull(splitCiteElementResult.GetNextContent());
  if (leftCiteElement && leftCiteElement->IsHTMLElement(nsGkAtoms::span) &&
      // XXX Oh, this depends on layout information of new element, and it's
      //     created by the hacky flush in DoSplitNode().  So we need to
      //     redesign around this for bug 1710784.
      leftCiteElement->GetPrimaryFrame() &&
      leftCiteElement->GetPrimaryFrame()->IsBlockFrameOrSubclass()) {
    nsIContent* lastChild = leftCiteElement->GetLastChild();
    if (lastChild && !lastChild->IsHTMLElement(nsGkAtoms::br)) {
      const CreateElementResult insertInvisibleBRElementResult =
          InsertBRElement(WithTransaction::Yes,
                          EditorDOMPoint::AtEndOf(*leftCiteElement));
      if (insertInvisibleBRElementResult.isErr()) {
        NS_WARNING("HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
        return Err(insertInvisibleBRElementResult.unwrapErr());
      }
      // We don't need to update selection here because we'll do another
      // InsertBRElement call soon.
      insertInvisibleBRElementResult.IgnoreCaretPointSuggestion();
      MOZ_ASSERT(insertInvisibleBRElementResult.GetNewNode());
    }
  }

  // In most cases, <br> should be inserted after current cite.  However, if
  // left cite hasn't been created because the split point was start of the
  // cite node, <br> should be inserted before the current cite.
  CreateElementResult insertBRElementResult =
      InsertBRElement(WithTransaction::Yes,
                      splitCiteElementResult.AtSplitPoint<EditorDOMPoint>());
  if (insertBRElementResult.isErr()) {
    NS_WARNING("HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
    return Err(insertBRElementResult.unwrapErr());
  }
  // We'll return with suggesting caret position.  Therefore, we don't need
  // to update selection here.
  insertBRElementResult.IgnoreCaretPointSuggestion();
  MOZ_ASSERT(insertBRElementResult.GetNewNode());

  // if aMailCiteElement wasn't a block, we might also want another break before
  // it. We need to examine the content both before the br we just added and
  // also just after it.  If we don't have another br or block boundary
  // adjacent, then we will need a 2nd br added to achieve blank line that user
  // expects.
  if (HTMLEditUtils::IsInlineElement(aMailCiteElement)) {
    nsresult rvOfInsertingBRElement = [&]() MOZ_CAN_RUN_SCRIPT {
      EditorDOMPoint pointToCreateNewBRElement(
          insertBRElementResult.GetNewNode());

      // XXX Cannot we replace this complicated check with just a call of
      //     HTMLEditUtils::IsVisibleBRElement with
      //     resultOfInsertingBRElement.inspect()?
      WSScanResult backwardScanFromPointToCreateNewBRElementResult =
          WSRunScanner::ScanPreviousVisibleNodeOrBlockBoundary(
              &aEditingHost, pointToCreateNewBRElement);
      if (MOZ_UNLIKELY(
              backwardScanFromPointToCreateNewBRElementResult.Failed())) {
        NS_WARNING(
            "WSRunScanner::ScanPreviousVisibleNodeOrBlockBoundary() "
            "failed");
        return NS_ERROR_FAILURE;
      }
      if (!backwardScanFromPointToCreateNewBRElementResult
               .InVisibleOrCollapsibleCharacters() &&
          !backwardScanFromPointToCreateNewBRElementResult
               .ReachedSpecialContent()) {
        return NS_SUCCESS_DOM_NO_OPERATION;
      }
      WSScanResult forwardScanFromPointAfterNewBRElementResult =
          WSRunScanner::ScanNextVisibleNodeOrBlockBoundary(
              &aEditingHost,
              EditorRawDOMPoint::After(pointToCreateNewBRElement));
      if (MOZ_UNLIKELY(forwardScanFromPointAfterNewBRElementResult.Failed())) {
        NS_WARNING("WSRunScanner::ScanNextVisibleNodeOrBlockBoundary() failed");
        return NS_ERROR_FAILURE;
      }
      if (!forwardScanFromPointAfterNewBRElementResult
               .InVisibleOrCollapsibleCharacters() &&
          !forwardScanFromPointAfterNewBRElementResult
               .ReachedSpecialContent() &&
          // In case we're at the very end.
          !forwardScanFromPointAfterNewBRElementResult
               .ReachedCurrentBlockBoundary()) {
        return NS_SUCCESS_DOM_NO_OPERATION;
      }
      CreateElementResult insertBRElementResult =
          InsertBRElement(WithTransaction::Yes, pointToCreateNewBRElement);
      if (insertBRElementResult.isErr()) {
        NS_WARNING("HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
        return insertBRElementResult.unwrapErr();
      }
      insertBRElementResult.IgnoreCaretPointSuggestion();
      MOZ_ASSERT(insertBRElementResult.GetNewNode());
      return NS_OK;
    }();

    if (NS_FAILED(rvOfInsertingBRElement)) {
      NS_WARNING(
          "Failed to insert additional <br> element before the inline right "
          "mail-cite element");
      return Err(rvOfInsertingBRElement);
    }
  }

  if (leftCiteElement && HTMLEditUtils::IsEmptyNode(*leftCiteElement)) {
    // MOZ_KnownLive(leftCiteElement) because it's grabbed by
    // splitCiteElementResult.
    nsresult rv = DeleteNodeWithTransaction(MOZ_KnownLive(*leftCiteElement));
    if (NS_FAILED(rv)) {
      NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
      return Err(rv);
    }
  }

  if (rightCiteElement && HTMLEditUtils::IsEmptyNode(*rightCiteElement)) {
    // MOZ_KnownLive(rightCiteElement) because it's grabbed by
    // splitCiteElementResult.
    nsresult rv = DeleteNodeWithTransaction(MOZ_KnownLive(*rightCiteElement));
    if (NS_FAILED(rv)) {
      NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
      return Err(rv);
    }
  }

  if (MOZ_UNLIKELY(!insertBRElementResult.GetNewNode()->GetParent())) {
    NS_WARNING("Inserted <br> shouldn't become an orphan node");
    return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }
  return EditorDOMPoint(insertBRElementResult.GetNewNode());
}

HTMLEditor::CharPointData
HTMLEditor::GetPreviousCharPointDataForNormalizingWhiteSpaces(
    const EditorDOMPointInText& aPoint) const {
  MOZ_ASSERT(aPoint.IsSetAndValid());

  if (!aPoint.IsStartOfContainer()) {
    return CharPointData::InSameTextNode(
        HTMLEditor::GetPreviousCharPointType(aPoint));
  }
  const auto previousCharPoint =
      WSRunScanner::GetPreviousEditableCharPoint<EditorRawDOMPointInText>(
          ComputeEditingHost(), aPoint);
  if (!previousCharPoint.IsSet()) {
    return CharPointData::InDifferentTextNode(CharPointType::TextEnd);
  }
  return CharPointData::InDifferentTextNode(
      HTMLEditor::GetCharPointType(previousCharPoint));
}

HTMLEditor::CharPointData
HTMLEditor::GetInclusiveNextCharPointDataForNormalizingWhiteSpaces(
    const EditorDOMPointInText& aPoint) const {
  MOZ_ASSERT(aPoint.IsSetAndValid());

  if (!aPoint.IsEndOfContainer()) {
    return CharPointData::InSameTextNode(HTMLEditor::GetCharPointType(aPoint));
  }
  const auto nextCharPoint =
      WSRunScanner::GetInclusiveNextEditableCharPoint<EditorRawDOMPointInText>(
          ComputeEditingHost(), aPoint);
  if (!nextCharPoint.IsSet()) {
    return CharPointData::InDifferentTextNode(CharPointType::TextEnd);
  }
  return CharPointData::InDifferentTextNode(
      HTMLEditor::GetCharPointType(nextCharPoint));
}

// static
void HTMLEditor::GenerateWhiteSpaceSequence(
    nsAString& aResult, uint32_t aLength,
    const CharPointData& aPreviousCharPointData,
    const CharPointData& aNextCharPointData) {
  MOZ_ASSERT(aResult.IsEmpty());
  MOZ_ASSERT(aLength);
  // For now, this method does not assume that result will be append to
  // white-space sequence in the text node.
  MOZ_ASSERT(aPreviousCharPointData.AcrossTextNodeBoundary() ||
             !aPreviousCharPointData.IsCollapsibleWhiteSpace());
  // For now, this method does not assume that the result will be inserted
  // into white-space sequence nor start of white-space sequence.
  MOZ_ASSERT(aNextCharPointData.AcrossTextNodeBoundary() ||
             !aNextCharPointData.IsCollapsibleWhiteSpace());

  if (aLength == 1) {
    // Even if previous/next char is in different text node, we should put
    // an ASCII white-space between visible characters.
    // XXX This means that this does not allow to put an NBSP in HTML editor
    //     without preformatted style.  However, Chrome has same issue too.
    if (aPreviousCharPointData.Type() == CharPointType::VisibleChar &&
        aNextCharPointData.Type() == CharPointType::VisibleChar) {
      aResult.Assign(HTMLEditUtils::kSpace);
      return;
    }
    // If it's start or end of text, put an NBSP.
    if (aPreviousCharPointData.Type() == CharPointType::TextEnd ||
        aNextCharPointData.Type() == CharPointType::TextEnd) {
      aResult.Assign(HTMLEditUtils::kNBSP);
      return;
    }
    // If the character is next to a preformatted linefeed, we need to put
    // an NBSP for avoiding collapsed into the linefeed.
    if (aPreviousCharPointData.Type() == CharPointType::PreformattedLineBreak ||
        aNextCharPointData.Type() == CharPointType::PreformattedLineBreak) {
      aResult.Assign(HTMLEditUtils::kNBSP);
      return;
    }
    // Now, the white-space will be inserted to a white-space sequence, but not
    // end of text.  We can put an ASCII white-space only when both sides are
    // not ASCII white-spaces.
    aResult.Assign(
        aPreviousCharPointData.Type() == CharPointType::ASCIIWhiteSpace ||
                aNextCharPointData.Type() == CharPointType::ASCIIWhiteSpace
            ? HTMLEditUtils::kNBSP
            : HTMLEditUtils::kSpace);
    return;
  }

  // Generate pairs of NBSP and ASCII white-space.
  aResult.SetLength(aLength);
  bool appendNBSP = true;  // Basically, starts with an NBSP.
  char16_t* lastChar = aResult.EndWriting() - 1;
  for (char16_t* iter = aResult.BeginWriting(); iter != lastChar; iter++) {
    *iter = appendNBSP ? HTMLEditUtils::kNBSP : HTMLEditUtils::kSpace;
    appendNBSP = !appendNBSP;
  }

  // If the final one is expected to an NBSP, we can put an NBSP simply.
  if (appendNBSP) {
    *lastChar = HTMLEditUtils::kNBSP;
    return;
  }

  // If next char point is end of text node, an ASCII white-space or
  // preformatted linefeed, we need to put an NBSP.
  *lastChar =
      aNextCharPointData.AcrossTextNodeBoundary() ||
              aNextCharPointData.Type() == CharPointType::ASCIIWhiteSpace ||
              aNextCharPointData.Type() == CharPointType::PreformattedLineBreak
          ? HTMLEditUtils::kNBSP
          : HTMLEditUtils::kSpace;
}

void HTMLEditor::ExtendRangeToDeleteWithNormalizingWhiteSpaces(
    EditorDOMPointInText& aStartToDelete, EditorDOMPointInText& aEndToDelete,
    nsAString& aNormalizedWhiteSpacesInStartNode,
    nsAString& aNormalizedWhiteSpacesInEndNode) const {
  MOZ_ASSERT(aStartToDelete.IsSetAndValid());
  MOZ_ASSERT(aEndToDelete.IsSetAndValid());
  MOZ_ASSERT(aStartToDelete.EqualsOrIsBefore(aEndToDelete));
  MOZ_ASSERT(aNormalizedWhiteSpacesInStartNode.IsEmpty());
  MOZ_ASSERT(aNormalizedWhiteSpacesInEndNode.IsEmpty());

  // First, check whether there is surrounding white-spaces or not, and if there
  // are, check whether they are collapsible or not.  Note that we shouldn't
  // touch white-spaces in different text nodes for performance, but we need
  // adjacent text node's first or last character information in some cases.
  Element* editingHost = ComputeEditingHost();
  const EditorDOMPointInText precedingCharPoint =
      WSRunScanner::GetPreviousEditableCharPoint(editingHost, aStartToDelete);
  const EditorDOMPointInText followingCharPoint =
      WSRunScanner::GetInclusiveNextEditableCharPoint(editingHost,
                                                      aEndToDelete);
  // Blink-compat: Normalize white-spaces in first node only when not removing
  //               its last character or no text nodes follow the first node.
  //               If removing last character of first node and there are
  //               following text nodes, white-spaces in following text node are
  //               normalized instead.
  const bool removingLastCharOfStartNode =
      aStartToDelete.ContainerAs<Text>() != aEndToDelete.ContainerAs<Text>() ||
      (aEndToDelete.IsEndOfContainer() && followingCharPoint.IsSet());
  const bool maybeNormalizePrecedingWhiteSpaces =
      !removingLastCharOfStartNode && precedingCharPoint.IsSet() &&
      !precedingCharPoint.IsEndOfContainer() &&
      precedingCharPoint.ContainerAs<Text>() ==
          aStartToDelete.ContainerAs<Text>() &&
      precedingCharPoint.IsCharCollapsibleASCIISpaceOrNBSP();
  const bool maybeNormalizeFollowingWhiteSpaces =
      followingCharPoint.IsSet() && !followingCharPoint.IsEndOfContainer() &&
      (followingCharPoint.ContainerAs<Text>() ==
           aEndToDelete.ContainerAs<Text>() ||
       removingLastCharOfStartNode) &&
      followingCharPoint.IsCharCollapsibleASCIISpaceOrNBSP();

  if (!maybeNormalizePrecedingWhiteSpaces &&
      !maybeNormalizeFollowingWhiteSpaces) {
    return;  // There are no white-spaces.
  }

  // Next, consider the range to normalize.
  EditorDOMPointInText startToNormalize, endToNormalize;
  if (maybeNormalizePrecedingWhiteSpaces) {
    Maybe<uint32_t> previousCharOffsetOfWhiteSpaces =
        HTMLEditUtils::GetPreviousNonCollapsibleCharOffset(
            precedingCharPoint, {WalkTextOption::TreatNBSPsCollapsible});
    startToNormalize.Set(precedingCharPoint.ContainerAs<Text>(),
                         previousCharOffsetOfWhiteSpaces.isSome()
                             ? previousCharOffsetOfWhiteSpaces.value() + 1
                             : 0);
    MOZ_ASSERT(!startToNormalize.IsEndOfContainer());
  }
  if (maybeNormalizeFollowingWhiteSpaces) {
    Maybe<uint32_t> nextCharOffsetOfWhiteSpaces =
        HTMLEditUtils::GetInclusiveNextNonCollapsibleCharOffset(
            followingCharPoint, {WalkTextOption::TreatNBSPsCollapsible});
    if (nextCharOffsetOfWhiteSpaces.isSome()) {
      endToNormalize.Set(followingCharPoint.ContainerAs<Text>(),
                         nextCharOffsetOfWhiteSpaces.value());
    } else {
      endToNormalize.SetToEndOf(followingCharPoint.ContainerAs<Text>());
    }
    MOZ_ASSERT(!endToNormalize.IsStartOfContainer());
  }

  // Next, retrieve surrounding information of white-space sequence.
  // If we're removing first text node's last character, we need to
  // normalize white-spaces starts from another text node.  In this case,
  // we need to lie for avoiding assertion in GenerateWhiteSpaceSequence().
  CharPointData previousCharPointData =
      removingLastCharOfStartNode
          ? CharPointData::InDifferentTextNode(CharPointType::TextEnd)
          : GetPreviousCharPointDataForNormalizingWhiteSpaces(
                startToNormalize.IsSet() ? startToNormalize : aStartToDelete);
  CharPointData nextCharPointData =
      GetInclusiveNextCharPointDataForNormalizingWhiteSpaces(
          endToNormalize.IsSet() ? endToNormalize : aEndToDelete);

  // Next, compute number of white-spaces in start/end node.
  uint32_t lengthInStartNode = 0, lengthInEndNode = 0;
  if (startToNormalize.IsSet()) {
    MOZ_ASSERT(startToNormalize.ContainerAs<Text>() ==
               aStartToDelete.ContainerAs<Text>());
    lengthInStartNode = aStartToDelete.Offset() - startToNormalize.Offset();
    MOZ_ASSERT(lengthInStartNode);
  }
  if (endToNormalize.IsSet()) {
    lengthInEndNode =
        endToNormalize.ContainerAs<Text>() == aEndToDelete.ContainerAs<Text>()
            ? endToNormalize.Offset() - aEndToDelete.Offset()
            : endToNormalize.Offset();
    MOZ_ASSERT(lengthInEndNode);
    // If we normalize white-spaces in a text node, we can replace all of them
    // with one ReplaceTextTransaction.
    if (endToNormalize.ContainerAs<Text>() ==
        aStartToDelete.ContainerAs<Text>()) {
      lengthInStartNode += lengthInEndNode;
      lengthInEndNode = 0;
    }
  }

  MOZ_ASSERT(lengthInStartNode + lengthInEndNode);

  // Next, generate normalized white-spaces.
  if (!lengthInEndNode) {
    HTMLEditor::GenerateWhiteSpaceSequence(
        aNormalizedWhiteSpacesInStartNode, lengthInStartNode,
        previousCharPointData, nextCharPointData);
  } else if (!lengthInStartNode) {
    HTMLEditor::GenerateWhiteSpaceSequence(
        aNormalizedWhiteSpacesInEndNode, lengthInEndNode, previousCharPointData,
        nextCharPointData);
  } else {
    // For making `GenerateWhiteSpaceSequence()` simpler, we should create
    // whole white-space sequence first, then, copy to the out params.
    nsAutoString whiteSpaces;
    HTMLEditor::GenerateWhiteSpaceSequence(
        whiteSpaces, lengthInStartNode + lengthInEndNode, previousCharPointData,
        nextCharPointData);
    aNormalizedWhiteSpacesInStartNode =
        Substring(whiteSpaces, 0, lengthInStartNode);
    aNormalizedWhiteSpacesInEndNode = Substring(whiteSpaces, lengthInStartNode);
    MOZ_ASSERT(aNormalizedWhiteSpacesInEndNode.Length() == lengthInEndNode);
  }

  // TODO: Shrink the replacing range and string as far as possible because
  //       this may run a lot, i.e., HTMLEditor creates ReplaceTextTransaction
  //       a lot for normalizing white-spaces.  Then, each transaction shouldn't
  //       have all white-spaces every time because once it's normalized, we
  //       don't need to normalize all of the sequence again, but currently
  //       we do.

  // Finally, extend the range.
  if (startToNormalize.IsSet()) {
    aStartToDelete = startToNormalize;
  }
  if (endToNormalize.IsSet()) {
    aEndToDelete = endToNormalize;
  }
}

Result<EditorDOMPoint, nsresult>
HTMLEditor::DeleteTextAndNormalizeSurroundingWhiteSpaces(
    const EditorDOMPointInText& aStartToDelete,
    const EditorDOMPointInText& aEndToDelete,
    TreatEmptyTextNodes aTreatEmptyTextNodes,
    DeleteDirection aDeleteDirection) {
  MOZ_ASSERT(aStartToDelete.IsSetAndValid());
  MOZ_ASSERT(aEndToDelete.IsSetAndValid());
  MOZ_ASSERT(aStartToDelete.EqualsOrIsBefore(aEndToDelete));

  // Use nsString for these replacing string because we should avoid to copy
  // the buffer from auto storange to ReplaceTextTransaction.
  nsString normalizedWhiteSpacesInFirstNode, normalizedWhiteSpacesInLastNode;

  // First, check whether we need to normalize white-spaces after deleting
  // the given range.
  EditorDOMPointInText startToDelete(aStartToDelete);
  EditorDOMPointInText endToDelete(aEndToDelete);
  ExtendRangeToDeleteWithNormalizingWhiteSpaces(
      startToDelete, endToDelete, normalizedWhiteSpacesInFirstNode,
      normalizedWhiteSpacesInLastNode);

  // If extended range is still collapsed, i.e., the caller just wants to
  // normalize white-space sequence, but there is no white-spaces which need to
  // be replaced, we need to do nothing here.
  if (startToDelete == endToDelete) {
    return aStartToDelete.To<EditorDOMPoint>();
  }

  // Note that the container text node of startToDelete may be removed from
  // the tree if it becomes empty.  Therefore, we need to track the point.
  EditorDOMPoint newCaretPosition;
  if (aStartToDelete.ContainerAs<Text>() == aEndToDelete.ContainerAs<Text>()) {
    newCaretPosition = aEndToDelete.To<EditorDOMPoint>();
  } else if (aDeleteDirection == DeleteDirection::Forward) {
    newCaretPosition.SetToEndOf(aStartToDelete.ContainerAs<Text>());
  } else {
    newCaretPosition.Set(aEndToDelete.ContainerAs<Text>(), 0u);
  }

  // Then, modify the text nodes in the range.
  while (true) {
    AutoTrackDOMPoint trackingNewCaretPosition(RangeUpdaterRef(),
                                               &newCaretPosition);
    // Use ReplaceTextTransaction if we need to normalize white-spaces in
    // the first text node.
    if (!normalizedWhiteSpacesInFirstNode.IsEmpty()) {
      EditorDOMPoint trackingEndToDelete(endToDelete.ContainerAs<Text>(),
                                         endToDelete.Offset());
      {
        AutoTrackDOMPoint trackEndToDelete(RangeUpdaterRef(),
                                           &trackingEndToDelete);
        uint32_t lengthToReplaceInFirstTextNode =
            startToDelete.ContainerAs<Text>() ==
                    trackingEndToDelete.ContainerAs<Text>()
                ? trackingEndToDelete.Offset() - startToDelete.Offset()
                : startToDelete.ContainerAs<Text>()->TextLength() -
                      startToDelete.Offset();
        nsresult rv = ReplaceTextWithTransaction(
            MOZ_KnownLive(*startToDelete.ContainerAs<Text>()),
            startToDelete.Offset(), lengthToReplaceInFirstTextNode,
            normalizedWhiteSpacesInFirstNode);
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::ReplaceTextWithTransaction() failed");
          return Err(rv);
        }
        if (startToDelete.ContainerAs<Text>() ==
            trackingEndToDelete.ContainerAs<Text>()) {
          MOZ_ASSERT(normalizedWhiteSpacesInLastNode.IsEmpty());
          break;  // There is no more text which we need to delete.
        }
      }
      if (MayHaveMutationEventListeners(
              NS_EVENT_BITS_MUTATION_CHARACTERDATAMODIFIED) &&
          (NS_WARN_IF(!trackingEndToDelete.IsSetAndValid()) ||
           NS_WARN_IF(!trackingEndToDelete.IsInTextNode()))) {
        return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
      }
      MOZ_ASSERT(trackingEndToDelete.IsInTextNode());
      endToDelete.Set(trackingEndToDelete.ContainerAs<Text>(),
                      trackingEndToDelete.Offset());
      // If the remaining range was modified by mutation event listener,
      // we should stop handling the deletion.
      startToDelete =
          EditorDOMPointInText::AtEndOf(*startToDelete.ContainerAs<Text>());
      if (MayHaveMutationEventListeners(
              NS_EVENT_BITS_MUTATION_CHARACTERDATAMODIFIED) &&
          NS_WARN_IF(!startToDelete.IsBefore(endToDelete))) {
        return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
      }
    }
    // Delete ASCII whiteSpaces in the range simpley if there are some text
    // nodes which we don't need to replace their text.
    if (normalizedWhiteSpacesInLastNode.IsEmpty() ||
        startToDelete.ContainerAs<Text>() != endToDelete.ContainerAs<Text>()) {
      // If we need to replace text in the last text node, we should
      // delete text before its previous text node.
      EditorDOMPointInText endToDeleteExceptReplaceRange =
          normalizedWhiteSpacesInLastNode.IsEmpty()
              ? endToDelete
              : EditorDOMPointInText(endToDelete.ContainerAs<Text>(), 0);
      if (startToDelete != endToDeleteExceptReplaceRange) {
        nsresult rv = DeleteTextAndTextNodesWithTransaction(
            startToDelete, endToDeleteExceptReplaceRange, aTreatEmptyTextNodes);
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed");
          return Err(rv);
        }
        if (normalizedWhiteSpacesInLastNode.IsEmpty()) {
          break;  // There is no more text which we need to delete.
        }
        if (MayHaveMutationEventListeners(
                NS_EVENT_BITS_MUTATION_CHARACTERDATAMODIFIED |
                NS_EVENT_BITS_MUTATION_NODEREMOVED |
                NS_EVENT_BITS_MUTATION_NODEREMOVEDFROMDOCUMENT |
                NS_EVENT_BITS_MUTATION_SUBTREEMODIFIED) &&
            (NS_WARN_IF(!endToDeleteExceptReplaceRange.IsSetAndValid()) ||
             NS_WARN_IF(!endToDelete.IsSetAndValid()) ||
             NS_WARN_IF(endToDelete.IsStartOfContainer()))) {
          return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
        }
        // Then, replace the text in the last text node.
        startToDelete = endToDeleteExceptReplaceRange;
      }
    }

    // Replace ASCII whiteSpaces in the range and following character in the
    // last text node.
    MOZ_ASSERT(!normalizedWhiteSpacesInLastNode.IsEmpty());
    MOZ_ASSERT(startToDelete.ContainerAs<Text>() ==
               endToDelete.ContainerAs<Text>());
    nsresult rv = ReplaceTextWithTransaction(
        MOZ_KnownLive(*startToDelete.ContainerAs<Text>()),
        startToDelete.Offset(), endToDelete.Offset() - startToDelete.Offset(),
        normalizedWhiteSpacesInLastNode);
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::ReplaceTextWithTransaction() failed");
      return Err(rv);
    }
    break;
  }

  if (!newCaretPosition.IsSetAndValid() ||
      !newCaretPosition.GetContainer()->IsInComposedDoc()) {
    NS_WARNING(
        "HTMLEditor::DeleteTextAndNormalizeSurroundingWhiteSpaces() got lost "
        "the modifying line");
    return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  // Look for leaf node to put caret if we remove some empty inline ancestors
  // at new caret position.
  if (!newCaretPosition.IsInTextNode()) {
    if (const Element* editableBlockElementOrInlineEditingHost =
            HTMLEditUtils::GetInclusiveAncestorElement(
                *newCaretPosition.ContainerAs<nsIContent>(),
                HTMLEditUtils::
                    ClosestEditableBlockElementOrInlineEditingHost)) {
      Element* editingHost = ComputeEditingHost();
      // Try to put caret next to immediately after previous editable leaf.
      nsIContent* previousContent =
          HTMLEditUtils::GetPreviousLeafContentOrPreviousBlockElement(
              newCaretPosition, *editableBlockElementOrInlineEditingHost,
              {LeafNodeType::LeafNodeOrNonEditableNode}, editingHost);
      if (previousContent && !HTMLEditUtils::IsBlockElement(*previousContent)) {
        newCaretPosition =
            previousContent->IsText() ||
                    HTMLEditUtils::IsContainerNode(*previousContent)
                ? EditorDOMPoint::AtEndOf(*previousContent)
                : EditorDOMPoint::After(*previousContent);
      }
      // But if the point is very first of a block element or immediately after
      // a child block, look for next editable leaf instead.
      else if (nsIContent* nextContent =
                   HTMLEditUtils::GetNextLeafContentOrNextBlockElement(
                       newCaretPosition,
                       *editableBlockElementOrInlineEditingHost,
                       {LeafNodeType::LeafNodeOrNonEditableNode},
                       editingHost)) {
        newCaretPosition = nextContent->IsText() ||
                                   HTMLEditUtils::IsContainerNode(*nextContent)
                               ? EditorDOMPoint(nextContent, 0)
                               : EditorDOMPoint(nextContent);
      }
    }
  }

  // For compatibility with Blink, we should move caret to end of previous
  // text node if it's direct previous sibling of the first text node in the
  // range.
  if (newCaretPosition.IsStartOfContainer() &&
      newCaretPosition.IsInTextNode() &&
      newCaretPosition.GetContainer()->GetPreviousSibling() &&
      newCaretPosition.GetContainer()->GetPreviousSibling()->IsText()) {
    newCaretPosition.SetToEndOf(
        newCaretPosition.GetContainer()->GetPreviousSibling()->AsText());
  }

  {
    AutoTrackDOMPoint trackingNewCaretPosition(RangeUpdaterRef(),
                                               &newCaretPosition);
    nsresult rv = InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary(
        newCaretPosition);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::"
          "InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary() failed");
      return Err(rv);
    }
  }
  if (!newCaretPosition.IsSetAndValid()) {
    NS_WARNING("Inserting <br> element caused unexpected DOM tree");
    return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }
  return newCaretPosition;
}

nsresult HTMLEditor::InsertBRElementIfHardLineIsEmptyAndEndsWithBlockBoundary(
    const EditorDOMPoint& aPointToInsert) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aPointToInsert.IsSet());

  if (!aPointToInsert.IsInContentNode()) {
    return NS_OK;
  }

  // If container of the point is not in a block, we don't need to put a
  // `<br>` element here.
  if (!HTMLEditUtils::IsBlockElement(
          *aPointToInsert.ContainerAs<nsIContent>())) {
    return NS_OK;
  }

  WSRunScanner wsRunScanner(ComputeEditingHost(), aPointToInsert);
  // If the point is not start of a hard line, we don't need to put a `<br>`
  // element here.
  if (!wsRunScanner.StartsFromHardLineBreak()) {
    return NS_OK;
  }
  // If the point is not end of a hard line or the hard line does not end with
  // block boundary, we don't need to put a `<br>` element here.
  if (!wsRunScanner.EndsByBlockBoundary()) {
    return NS_OK;
  }

  // If we cannot insert a `<br>` element here, do nothing.
  if (!HTMLEditUtils::CanNodeContain(*aPointToInsert.GetContainer(),
                                     *nsGkAtoms::br)) {
    return NS_OK;
  }

  CreateElementResult insertBRElementResult = InsertBRElement(
      WithTransaction::Yes, aPointToInsert, nsIEditor::ePrevious);
  if (insertBRElementResult.isErr()) {
    NS_WARNING(
        "HTMLEditor::InsertBRElement(WithTransaction::Yes, ePrevious) failed");
    return insertBRElementResult.unwrapErr();
  }
  nsresult rv = insertBRElementResult.SuggestCaretPointTo(*this, {});
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "CreateElementResult::SuggestCaretPointTo() failed");
  return rv;
}

EditActionResult HTMLEditor::MakeOrChangeListAndListItemAsSubAction(
    nsAtom& aListElementOrListItemElementTagName, const nsAString& aBulletType,
    SelectAllOfCurrentList aSelectAllOfCurrentList) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(&aListElementOrListItemElementTagName == nsGkAtoms::ul ||
             &aListElementOrListItemElementTagName == nsGkAtoms::ol ||
             &aListElementOrListItemElementTagName == nsGkAtoms::dl ||
             &aListElementOrListItemElementTagName == nsGkAtoms::dd ||
             &aListElementOrListItemElementTagName == nsGkAtoms::dt);

  if (NS_WARN_IF(!mInitSucceeded)) {
    return EditActionIgnored(NS_ERROR_NOT_INITIALIZED);
  }

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Some selection containers are not content node, but ignored");
    return EditActionIgnored();
  }

  AutoPlaceholderBatch treatAsOneTransaction(
      *this, ScrollSelectionIntoView::Yes, __FUNCTION__);

  // XXX EditSubAction::eCreateOrChangeDefinitionListItem and
  //     EditSubAction::eCreateOrChangeList are treated differently in
  //     HTMLEditor::MaybeSplitElementsAtEveryBRElement().  Only when
  //     EditSubAction::eCreateOrChangeList, it splits inline nodes.
  //     Currently, it shouldn't be done when we called for formatting
  //     `<dd>` or `<dt>` by
  //     HTMLEditor::MakeDefinitionListItemWithTransaction().  But this
  //     difference may be a bug.  We should investigate this later.
  IgnoredErrorResult error;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this,
      &aListElementOrListItemElementTagName == nsGkAtoms::dd ||
              &aListElementOrListItemElementTagName == nsGkAtoms::dt
          ? EditSubAction::eCreateOrChangeDefinitionListItem
          : EditSubAction::eCreateOrChangeList,
      nsIEditor::eNext, error);
  if (NS_WARN_IF(error.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return EditActionResult(error.StealNSResult());
  }
  NS_WARNING_ASSERTION(
      !error.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (NS_SUCCEEDED(rv) && SelectionRef().IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterInvisibleBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterInvisibleBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  nsAtom* listTagName = nullptr;
  nsAtom* listItemTagName = nullptr;
  if (&aListElementOrListItemElementTagName == nsGkAtoms::ul ||
      &aListElementOrListItemElementTagName == nsGkAtoms::ol) {
    listTagName = &aListElementOrListItemElementTagName;
    listItemTagName = nsGkAtoms::li;
  } else if (&aListElementOrListItemElementTagName == nsGkAtoms::dl) {
    listTagName = &aListElementOrListItemElementTagName;
    listItemTagName = nsGkAtoms::dd;
  } else if (&aListElementOrListItemElementTagName == nsGkAtoms::dd ||
             &aListElementOrListItemElementTagName == nsGkAtoms::dt) {
    listTagName = nsGkAtoms::dl;
    listItemTagName = &aListElementOrListItemElementTagName;
  } else {
    NS_WARNING(
        "aListElementOrListItemElementTagName was neither list element name "
        "nor "
        "definition listitem element name");
    return EditActionResult(NS_ERROR_INVALID_ARG);
  }

  const RefPtr<Element> editingHost = ComputeEditingHost();
  if (MOZ_UNLIKELY(!editingHost)) {
    return EditActionIgnored(NS_SUCCESS_DOM_NO_OPERATION);
  }

  // Expands selection range to include the immediate block parent, and then
  // further expands to include any ancestors whose children are all in the
  // range.
  // XXX Why do we do this only when there is only one selection range?
  if (!SelectionRef().IsCollapsed() && SelectionRef().RangeCount() == 1u) {
    Result<EditorRawDOMRange, nsresult> extendedRange =
        GetRangeExtendedToHardLineEdgesForBlockEditAction(
            SelectionRef().GetRangeAt(0u), *editingHost);
    if (MOZ_UNLIKELY(extendedRange.isErr())) {
      NS_WARNING(
          "HTMLEditor::GetRangeExtendedToHardLineEdgesForBlockEditAction() "
          "failed");
      return EditActionResult(extendedRange.unwrapErr());
    }
    // Note that end point may be prior to start point.  So, we
    // cannot use Selection::SetStartAndEndInLimit() here.
    error.SuppressException();
    SelectionRef().SetBaseAndExtentInLimiter(
        extendedRange.inspect().StartRef().ToRawRangeBoundary(),
        extendedRange.inspect().EndRef().ToRawRangeBoundary(), error);
    if (NS_WARN_IF(Destroyed())) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    if (error.Failed()) {
      NS_WARNING("Selection::SetBaseAndExtentInLimiter() failed");
      return EditActionResult(error.StealNSResult());
    }
  }

  AutoRangeArray selectionRanges(SelectionRef());
  result = ConvertContentAroundRangesToList(
      selectionRanges, MOZ_KnownLive(*listTagName),
      MOZ_KnownLive(*listItemTagName), aBulletType, aSelectAllOfCurrentList,
      *editingHost);
  if (MOZ_UNLIKELY(result.Failed())) {
    NS_WARNING("HTMLEditor::ConvertContentAroundRangesToList() failed");
    // XXX Should we try to restore selection ranges in this case?
    return result;
  }

  rv = selectionRanges.ApplyTo(SelectionRef());
  if (NS_WARN_IF(Destroyed())) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::ApplyTo() failed");
  return result.Ignored() ? EditActionCanceled(rv) : EditActionHandled(rv);
}

EditActionResult HTMLEditor::ConvertContentAroundRangesToList(
    AutoRangeArray& aRanges, nsAtom& aListElementTagName,
    nsAtom& aListItemElementTagName, const nsAString& aBulletType,
    SelectAllOfCurrentList aSelectAllOfCurrentList,
    const Element& aEditingHost) {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(!IsSelectionRangeContainerNotContent());

  if (NS_WARN_IF(!aRanges.SaveAndTrackRanges(*this))) {
    return EditActionResult(NS_ERROR_FAILURE);
  }

  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;

  if (Element* parentListElement =
          aSelectAllOfCurrentList == SelectAllOfCurrentList::Yes
              ? aRanges.GetClosestAncestorAnyListElementOfRange()
              : nullptr) {
    arrayOfContents.AppendElement(
        OwningNonNull<nsIContent>(*parentListElement));
  } else {
    AutoRangeArray extendedRanges(aRanges);

    // TODO: We don't need AutoTransactionsConserveSelection here in the
    //       normal cases, but removing this may cause the behavior with the
    //       legacy mutation event listeners.  We should try to delete this in
    //       a bug.
    AutoTransactionsConserveSelection dontChangeMySelection(*this);

    extendedRanges.ExtendRangesToWrapLinesToHandleBlockLevelEditAction(
        EditSubAction::eCreateOrChangeList, aEditingHost);
    Result<EditorDOMPoint, nsresult> splitResult =
        extendedRanges
            .SplitTextNodesAtEndBoundariesAndParentInlineElementsAtBoundaries(
                *this);
    if (MOZ_UNLIKELY(splitResult.isErr())) {
      NS_WARNING(
          "AutoRangeArray::"
          "SplitTextNodesAtEndBoundariesAndParentInlineElementsAtBoundaries() "
          "failed");
      return EditActionResult(splitResult.unwrapErr());
    }
    nsresult rv = extendedRanges.CollectEditTargetNodes(
        *this, arrayOfContents, EditSubAction::eCreateOrChangeList,
        AutoRangeArray::CollectNonEditableNodes::No);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "AutoRangeArray::CollectEditTargetNodes(EditSubAction::"
          "eCreateOrChangeList, CollectNonEditableNodes::No) failed");
      return EditActionResult(rv);
    }

    const Result<EditorDOMPoint, nsresult> splitAtBRElementsResult =
        MaybeSplitElementsAtEveryBRElement(arrayOfContents,
                                           EditSubAction::eCreateOrChangeList);
    if (MOZ_UNLIKELY(splitAtBRElementsResult.isErr())) {
      NS_WARNING(
          "HTMLEditor::MaybeSplitElementsAtEveryBRElement(EditSubAction::"
          "eCreateOrChangeList) failed");
      return EditActionResult(splitAtBRElementsResult.inspectErr());
    }
  }

  // check if all our nodes are <br>s, or empty inlines
  auto IsEmptyOrContainsOnlyBRElementsOrEmptyInlineElements =
      [](const nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents) {
        for (auto& content : aArrayOfContents) {
          // if content is not a Break or empty inline, we're done
          if (!content->IsHTMLElement(nsGkAtoms::br) &&
              !HTMLEditUtils::IsEmptyInlineContainer(
                  content, {EmptyCheckOption::TreatSingleBRElementAsVisible})) {
            return false;
          }
        }
        return true;
      };

  // if no nodes, we make empty list.  Ditto if the user tried to make a list
  // of some # of breaks.
  if (IsEmptyOrContainsOnlyBRElementsOrEmptyInlineElements(arrayOfContents)) {
    // if only breaks, delete them
    for (auto& content : arrayOfContents) {
      // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
      // keep it alive.
      nsresult rv = DeleteNodeWithTransaction(MOZ_KnownLive(*content));
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
        return EditActionResult(rv);
      }
    }

    const auto firstRangeStartPoint =
        aRanges.GetFirstRangeStartPoint<EditorDOMPoint>();
    if (NS_WARN_IF(!firstRangeStartPoint.IsSet())) {
      return EditActionResult(NS_ERROR_FAILURE);
    }

    // Make sure we can put a list here.
    if (!HTMLEditUtils::CanNodeContain(*firstRangeStartPoint.GetContainer(),
                                       aListElementTagName)) {
      aRanges.RestoreFromSavedRanges();
      return EditActionCanceled();
    }

    RefPtr<Element> newListItemElement;
    CreateElementResult createNewListElementResult =
        InsertElementWithSplittingAncestorsWithTransaction(
            aListElementTagName, firstRangeStartPoint,
            BRElementNextToSplitPoint::Keep, aEditingHost,
            // MOZ_CAN_RUN_SCRIPT_BOUNDARY due to bug 1758868
            [&newListItemElement, &aListItemElementTagName](
                HTMLEditor& aHTMLEditor, Element& aListElement,
                const EditorDOMPoint& aPointToInsert)
                MOZ_CAN_RUN_SCRIPT_BOUNDARY {
                  const auto withTransaction = aListElement.IsInComposedDoc()
                                                   ? WithTransaction::Yes
                                                   : WithTransaction::No;
                  CreateElementResult createNewListItemElementResult =
                      aHTMLEditor.CreateAndInsertElement(
                          withTransaction, aListItemElementTagName,
                          EditorDOMPoint(&aListElement, 0u));
                  if (createNewListItemElementResult.isErr()) {
                    NS_WARNING(
                        nsPrintfCString(
                            "HTMLEditor::CreateAndInsertElement(%s) failed",
                            ToString(withTransaction).c_str())
                            .get());
                    return createNewListItemElementResult.unwrapErr();
                  }
                  // There is AutoSelectionRestorer in this method so that it'll
                  // be restored or updated with making it abort.  Therefore,
                  // we don't need to update selection here.
                  // XXX I'd like to check aRanges.HasSavedRanges() here, but it
                  //     requires ifdefs to avoid bustage of opt builds caused
                  //     by unused warning...
                  createNewListItemElementResult.IgnoreCaretPointSuggestion();
                  newListItemElement =
                      createNewListItemElementResult.UnwrapNewNode();
                  MOZ_ASSERT(newListItemElement);
                  return NS_OK;
                });
    if (createNewListElementResult.isErr()) {
      NS_WARNING(
          nsPrintfCString(
              "HTMLEditor::InsertElementWithSplittingAncestorsWithTransaction("
              "%s) failed",
              nsAtomCString(&aListElementTagName).get())
              .get());
      return EditActionResult(createNewListElementResult.unwrapErr());
    }
    MOZ_ASSERT(createNewListElementResult.GetNewNode());

    // Put selection in new list item and don't restore the Selection.
    createNewListElementResult.IgnoreCaretPointSuggestion();
    aRanges.ClearSavedRanges();
    nsresult rv = aRanges.Collapse(EditorRawDOMPoint(newListItemElement, 0u));
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::Collapse() failed");
    return EditActionResult(rv);
  }

  // if there is only one node in the array, and it is a list, div, or
  // blockquote, then look inside of it until we find inner list or content.
  if (arrayOfContents.Length() == 1) {
    if (Element* deepestDivBlockquoteOrListElement =
            HTMLEditUtils::GetInclusiveDeepestFirstChildWhichHasOneChild(
                arrayOfContents[0], {WalkTreeOption::IgnoreNonEditableNode},
                nsGkAtoms::div, nsGkAtoms::blockquote, nsGkAtoms::ul,
                nsGkAtoms::ol, nsGkAtoms::dl)) {
      if (deepestDivBlockquoteOrListElement->IsAnyOfHTMLElements(
              nsGkAtoms::div, nsGkAtoms::blockquote)) {
        arrayOfContents.Clear();
        HTMLEditUtils::CollectChildren(*deepestDivBlockquoteOrListElement,
                                       arrayOfContents, 0, {});
      } else {
        arrayOfContents.ReplaceElementAt(
            0, OwningNonNull<nsIContent>(*deepestDivBlockquoteOrListElement));
      }
    }
  }

  // Ok, now go through all the nodes and put then in the list,
  // or whatever is appropriate.  Wohoo!

  uint32_t countOfCollectedContents = arrayOfContents.Length();
  RefPtr<Element> curList, prevListItem, listItemOrListToPutCaret;

  for (uint32_t i = 0; i < countOfCollectedContents; i++) {
    // here's where we actually figure out what to do
    const OwningNonNull<nsIContent> content = arrayOfContents[i];

    // make sure we don't assemble content that is in different table cells
    // into the same list.  respect table cell boundaries when listifying.
    if (curList &&
        HTMLEditUtils::GetInclusiveAncestorAnyTableElement(*curList) !=
            HTMLEditUtils::GetInclusiveAncestorAnyTableElement(content)) {
      curList = nullptr;
    }

    // If current node is a `<br>` element, delete it and forget previous
    // list item element.
    // If current node is an empty inline node, just delete it.
    if (EditorUtils::IsEditableContent(content, EditorType::HTML) &&
        (content->IsHTMLElement(nsGkAtoms::br) ||
         HTMLEditUtils::IsEmptyInlineContainer(
             content, {EmptyCheckOption::TreatSingleBRElementAsVisible}))) {
      nsresult rv = DeleteNodeWithTransaction(*content);
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
        return EditActionResult(rv);
      }
      if (content->IsHTMLElement(nsGkAtoms::br)) {
        prevListItem = nullptr;
      }
      continue;
    }

    if (HTMLEditUtils::IsAnyListElement(content)) {
      // If we met a list element and current list element is not a descendant
      // of the list, append current node to end of the current list element.
      // Then, wrap it with list item element and delete the old container.
      if (curList && !EditorUtils::IsDescendantOf(*content, *curList)) {
        const MoveNodeResult moveNodeResult =
            MoveNodeToEndWithTransaction(*content, *curList);
        if (moveNodeResult.isErr()) {
          NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
          return EditActionResult(moveNodeResult.inspectErr());
        }
        MOZ_ASSERT(aRanges.HasSavedRanges());
        moveNodeResult.IgnoreCaretPointSuggestion();

        const CreateElementResult convertListTypeResult =
            ChangeListElementType(MOZ_KnownLive(*content->AsElement()),
                                  aListElementTagName, aListItemElementTagName);
        if (convertListTypeResult.isErr()) {
          NS_WARNING("HTMLEditor::ChangeListElementType() failed");
          return EditActionResult(convertListTypeResult.inspectErr());
        }
        MOZ_ASSERT(aRanges.HasSavedRanges());
        convertListTypeResult.IgnoreCaretPointSuggestion();

        const Result<EditorDOMPoint, nsresult> unwrapNewListElementResult =
            RemoveBlockContainerWithTransaction(
                MOZ_KnownLive(*convertListTypeResult.GetNewNode()));
        if (MOZ_UNLIKELY(unwrapNewListElementResult.isErr())) {
          NS_WARNING(
              "HTMLEditor::RemoveBlockContainerWithTransaction() failed");
          return EditActionResult(unwrapNewListElementResult.inspectErr());
        }
        MOZ_ASSERT(aRanges.HasSavedRanges());  // So, ignore the suggested point

        prevListItem = nullptr;
        continue;
      }

      // If current list element is in found list element or we've not met a
      // list element, convert current list element to proper type.
      CreateElementResult convertListTypeResult =
          ChangeListElementType(MOZ_KnownLive(*content->AsElement()),
                                aListElementTagName, aListItemElementTagName);
      if (convertListTypeResult.isErr()) {
        NS_WARNING("HTMLEditor::ChangeListElementType() failed");
        return EditActionResult(convertListTypeResult.unwrapErr());
      }
      MOZ_ASSERT(aRanges.HasSavedRanges());
      convertListTypeResult.IgnoreCaretPointSuggestion();
      MOZ_ASSERT(convertListTypeResult.GetNewNode());
      curList = convertListTypeResult.UnwrapNewNode();
      prevListItem = nullptr;
      continue;
    }

    EditorDOMPoint atContent(content);
    if (NS_WARN_IF(!atContent.IsSet())) {
      return EditActionResult(NS_ERROR_FAILURE);
    }
    MOZ_ASSERT(atContent.IsSetAndValid());
    if (HTMLEditUtils::IsListItem(content)) {
      // If current list item element is not in proper list element, we need
      // to convert the list element.
      if (!atContent.IsContainerHTMLElement(&aListElementTagName)) {
        // If we've not met a list element or current node is not in current
        // list element, insert a list element at current node and set
        // current list element to the new one.
        if (!curList || EditorUtils::IsDescendantOf(*content, *curList)) {
          if (NS_WARN_IF(!atContent.IsInContentNode())) {
            return EditActionResult(NS_ERROR_FAILURE);
          }
          const SplitNodeResult splitListItemParentResult =
              SplitNodeWithTransaction(atContent);
          if (splitListItemParentResult.isErr()) {
            NS_WARNING("HTMLEditor::SplitNodeWithTransaction() failed");
            return EditActionResult(splitListItemParentResult.inspectErr());
          }
          MOZ_ASSERT(splitListItemParentResult.DidSplit());
          MOZ_ASSERT(aRanges.HasSavedRanges());
          splitListItemParentResult.IgnoreCaretPointSuggestion();

          CreateElementResult createNewListElementResult =
              CreateAndInsertElement(
                  WithTransaction::Yes, aListElementTagName,
                  splitListItemParentResult.AtNextContent<EditorDOMPoint>());
          if (createNewListElementResult.isErr()) {
            NS_WARNING(
                "HTMLEditor::CreateAndInsertElement(WithTransaction::Yes) "
                "failed");
            return EditActionResult(createNewListElementResult.unwrapErr());
          }
          MOZ_ASSERT(aRanges.HasSavedRanges());
          createNewListElementResult.IgnoreCaretPointSuggestion();
          MOZ_ASSERT(createNewListElementResult.GetNewNode());
          curList = createNewListElementResult.UnwrapNewNode();
        }
        // Then, move current node into current list element.
        const MoveNodeResult moveNodeResult =
            MoveNodeToEndWithTransaction(*content, *curList);
        if (moveNodeResult.isErr()) {
          NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
          return EditActionResult(moveNodeResult.unwrapErr());
        }
        MOZ_ASSERT(aRanges.HasSavedRanges());
        moveNodeResult.IgnoreCaretPointSuggestion();

        // Convert list item type if current node is different list item type.
        if (!content->IsHTMLElement(&aListItemElementTagName)) {
          const CreateElementResult newListItemElementOrError =
              ReplaceContainerWithTransaction(
                  MOZ_KnownLive(*content->AsElement()),
                  aListItemElementTagName);
          if (newListItemElementOrError.isErr()) {
            NS_WARNING("HTMLEditor::ReplaceContainerWithTransaction() failed");
            return EditActionResult(newListItemElementOrError.inspectErr());
          }
          MOZ_ASSERT(aRanges.HasSavedRanges());
          newListItemElementOrError.IgnoreCaretPointSuggestion();
        }
      } else {
        // If we've not met a list element, set current list element to the
        // parent of current list item element.
        if (!curList) {
          curList = atContent.GetContainerAs<Element>();
          NS_WARNING_ASSERTION(
              HTMLEditUtils::IsAnyListElement(curList),
              "Current list item parent is not a list element");
        }
        // If current list item element is not a child of current list element,
        // move it into current list item.
        else if (atContent.GetContainer() != curList) {
          const MoveNodeResult moveNodeResult =
              MoveNodeToEndWithTransaction(*content, *curList);
          if (moveNodeResult.isErr()) {
            NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
            return EditActionResult(moveNodeResult.inspectErr());
          }
          MOZ_ASSERT(aRanges.HasSavedRanges());
          moveNodeResult.IgnoreCaretPointSuggestion();
        }
        // Then, if current list item element is not proper type for current
        // list element, convert list item element to proper element.
        if (!content->IsHTMLElement(&aListItemElementTagName)) {
          const CreateElementResult newListItemElementOrError =
              ReplaceContainerWithTransaction(
                  MOZ_KnownLive(*content->AsElement()),
                  aListItemElementTagName);
          if (newListItemElementOrError.isErr()) {
            NS_WARNING("HTMLEditor::ReplaceContainerWithTransaction() failed");
            return EditActionResult(newListItemElementOrError.inspectErr());
          }
          MOZ_ASSERT(aRanges.HasSavedRanges());
          newListItemElementOrError.IgnoreCaretPointSuggestion();
        }
      }
      Element* element = Element::FromNode(content);
      if (NS_WARN_IF(!element)) {
        return EditActionResult(NS_ERROR_FAILURE);
      }
      // If bullet type is specified, set list type attribute.
      // XXX Cannot we set type attribute before inserting the list item
      //     element into the DOM tree?
      if (!aBulletType.IsEmpty()) {
        nsresult rv = SetAttributeWithTransaction(
            MOZ_KnownLive(*element), *nsGkAtoms::type, aBulletType);
        if (NS_WARN_IF(Destroyed())) {
          return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
        }
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "EditorBase::SetAttributeWithTransaction(nsGkAtoms::type) "
              "failed");
          return EditActionResult(rv);
        }
        continue;
      }

      // Otherwise, remove list type attribute if there is.
      if (!element->HasAttr(nsGkAtoms::type)) {
        continue;
      }
      nsresult rv = RemoveAttributeWithTransaction(MOZ_KnownLive(*element),
                                                   *nsGkAtoms::type);
      if (NS_WARN_IF(Destroyed())) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "EditorBase::RemoveAttributeWithTransaction(nsGkAtoms::type) "
            "failed");
        return EditActionResult(rv);
      }
      continue;
    }

    MOZ_ASSERT(!HTMLEditUtils::IsAnyListElement(content) &&
               !HTMLEditUtils::IsListItem(content));

    // If current node is a `<div>` element, replace it in the array with
    // its children.
    // XXX I think that this should be done when we collect the nodes above.
    //     Then, we can change this `for` loop to ranged-for loop.
    if (content->IsHTMLElement(nsGkAtoms::div)) {
      prevListItem = nullptr;
      HTMLEditUtils::CollectChildren(
          *content, arrayOfContents, i + 1,
          {CollectChildrenOption::CollectListChildren,
           CollectChildrenOption::CollectTableChildren});
      const Result<EditorDOMPoint, nsresult> unwrapDivElementResult =
          RemoveContainerWithTransaction(MOZ_KnownLive(*content->AsElement()));
      if (MOZ_UNLIKELY(unwrapDivElementResult.isErr())) {
        NS_WARNING("HTMLEditor::RemoveContainerWithTransaction() failed");
        return EditActionResult(unwrapDivElementResult.inspectErr());
      }
      MOZ_ASSERT(aRanges.HasSavedRanges());  // So, ignore the suggested point

      // Extend the loop length to handle all children collected here.
      countOfCollectedContents = arrayOfContents.Length();
      continue;
    }

    // If we've not met a list element, create a list element and make it
    // current list element.
    if (!curList) {
      prevListItem = nullptr;
      CreateElementResult createNewListElementResult =
          InsertElementWithSplittingAncestorsWithTransaction(
              aListElementTagName, atContent, BRElementNextToSplitPoint::Keep,
              aEditingHost);
      if (createNewListElementResult.isErr()) {
        NS_WARNING(
            nsPrintfCString(
                "HTMLEditor::"
                "InsertElementWithSplittingAncestorsWithTransaction(%s) failed",
                nsAtomCString(&aListElementTagName).get())
                .get());
        return EditActionResult(createNewListElementResult.unwrapErr());
      }
      MOZ_ASSERT(aRanges.HasSavedRanges());
      createNewListElementResult.IgnoreCaretPointSuggestion();

      MOZ_ASSERT(createNewListElementResult.GetNewNode());
      listItemOrListToPutCaret = createNewListElementResult.GetNewNode();
      curList = createNewListElementResult.UnwrapNewNode();

      // atContent is now referring the right node with mOffset but
      // referring the left node with mRef.  So, invalidate it now.
      atContent.Clear();
    }

    // If we're currently handling contents of a list item and current node
    // is not a block element, move current node into the list item.
    if (HTMLEditUtils::IsInlineElement(content) && prevListItem) {
      const MoveNodeResult moveInlineElementResult =
          MoveNodeToEndWithTransaction(*content, *prevListItem);
      if (moveInlineElementResult.isErr()) {
        NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
        return EditActionResult(moveInlineElementResult.unwrapErr());
      }
      MOZ_ASSERT(aRanges.HasSavedRanges());
      moveInlineElementResult.IgnoreCaretPointSuggestion();
      continue;
    }

    // If current node is a paragraph, that means that it does not contain
    // block children so that we can just replace it with new list item
    // element and move it into current list element.
    // XXX This is too rough handling.  If web apps modifies DOM tree directly,
    //     any elements can have block elements as children.
    if (content->IsHTMLElement(nsGkAtoms::p)) {
      CreateElementResult newListItemElementOrError =
          ReplaceContainerWithTransaction(MOZ_KnownLive(*content->AsElement()),
                                          aListItemElementTagName);
      if (newListItemElementOrError.isErr()) {
        NS_WARNING("HTMLEditor::ReplaceContainerWithTransaction() failed");
        return EditActionResult(newListItemElementOrError.unwrapErr());
      }
      MOZ_ASSERT(aRanges.HasSavedRanges());
      newListItemElementOrError.IgnoreCaretPointSuggestion();
      MOZ_ASSERT(newListItemElementOrError.GetNewNode());

      const MoveNodeResult moveListItemElementResult =
          MoveNodeToEndWithTransaction(
              MOZ_KnownLive(*newListItemElementOrError.GetNewNode()), *curList);
      if (moveListItemElementResult.isErr()) {
        NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
        return EditActionResult(moveListItemElementResult.unwrapErr());
      }
      MOZ_ASSERT(aRanges.HasSavedRanges());
      moveListItemElementResult.IgnoreCaretPointSuggestion();

      prevListItem = nullptr;
      // XXX Why don't we set `type` attribute here??
      continue;
    }

    // If current node is not a paragraph, wrap current node with new list
    // item element and move it into current list element.
    CreateElementResult wrapContentInListItemElementResult =
        InsertContainerWithTransaction(*content, aListItemElementTagName);
    if (wrapContentInListItemElementResult.isErr()) {
      NS_WARNING("HTMLEditor::InsertContainerWithTransaction() failed");
      return EditActionResult(wrapContentInListItemElementResult.unwrapErr());
    }
    MOZ_ASSERT(aRanges.HasSavedRanges());
    wrapContentInListItemElementResult.IgnoreCaretPointSuggestion();
    MOZ_ASSERT(wrapContentInListItemElementResult.GetNewNode());

    // MOZ_KnownLive(wrapContentInListItemElementResult.GetNewNode()): The
    // result is grabbed by wrapContentInListItemElementResult.
    const MoveNodeResult moveListItemElementResult =
        MoveNodeToEndWithTransaction(
            MOZ_KnownLive(*wrapContentInListItemElementResult.GetNewNode()),
            *curList);
    if (moveListItemElementResult.isErr()) {
      NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
      return EditActionResult(moveListItemElementResult.inspectErr());
    }
    MOZ_ASSERT(aRanges.HasSavedRanges());
    moveListItemElementResult.IgnoreCaretPointSuggestion();

    // If current node is not a block element, new list item should have
    // following inline nodes too.
    if (HTMLEditUtils::IsInlineElement(content)) {
      prevListItem = wrapContentInListItemElementResult.UnwrapNewNode();
    } else {
      prevListItem = nullptr;
    }

    // XXX Why don't we set `type` attribute here??
  }

  MOZ_ASSERT(aRanges.HasSavedRanges());
  aRanges.RestoreFromSavedRanges();

  // If selection will be collapsed but not in listItemOrListToPutCaret, we need
  // to adjust the caret position into it.
  if (listItemOrListToPutCaret && aRanges.IsCollapsed() &&
      aRanges.Ranges().Length()) {
    const auto firstRangeStartPoint =
        aRanges.GetFirstRangeStartPoint<EditorRawDOMPoint>();
    if (MOZ_LIKELY(firstRangeStartPoint.IsSet())) {
      const Result<EditorRawDOMPoint, nsresult> pointToPutCaretOrError =
          HTMLEditUtils::ComputePointToPutCaretInElementIfOutside<
              EditorRawDOMPoint>(*listItemOrListToPutCaret,
                                 firstRangeStartPoint);
      if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
        NS_WARNING(
            "HTMLEditUtils::ComputePointToPutCaretInElementIfOutside() "
            "failed, but ignored");
      } else if (pointToPutCaretOrError.inspect().IsSet()) {
        DebugOnly<nsresult> rvIgnored =
            aRanges.Collapse(pointToPutCaretOrError.inspect());
        NS_WARNING_ASSERTION(
            NS_SUCCEEDED(rvIgnored),
            "EditorBase::CollapseSelectionTo() failed, but ignored");
      }
    }
  }

  return EditActionHandled();
}

nsresult HTMLEditor::RemoveListAtSelectionAsSubAction(
    const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result.Rv();
  }

  AutoPlaceholderBatch treatAsOneTransaction(
      *this, ScrollSelectionIntoView::Yes, __FUNCTION__);
  IgnoredErrorResult error;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this, EditSubAction::eRemoveList, nsIEditor::eNext, error);
  if (NS_WARN_IF(error.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return error.StealNSResult();
  }
  NS_WARNING_ASSERTION(
      !error.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  // XXX Why do we do this only when there is only one selection range?
  if (!SelectionRef().IsCollapsed() && SelectionRef().RangeCount() == 1u) {
    Result<EditorRawDOMRange, nsresult> extendedRange =
        GetRangeExtendedToHardLineEdgesForBlockEditAction(
            SelectionRef().GetRangeAt(0u), aEditingHost);
    if (MOZ_UNLIKELY(extendedRange.isErr())) {
      NS_WARNING(
          "HTMLEditor::GetRangeExtendedToHardLineEdgesForBlockEditAction() "
          "failed");
      return extendedRange.unwrapErr();
    }
    // Note that end point may be prior to start point.  So, we
    // cannot use Selection::SetStartAndEndInLimit() here.
    error.SuppressException();
    SelectionRef().SetBaseAndExtentInLimiter(
        extendedRange.inspect().StartRef().ToRawRangeBoundary(),
        extendedRange.inspect().EndRef().ToRawRangeBoundary(), error);
    if (NS_WARN_IF(Destroyed())) {
      return NS_ERROR_EDITOR_DESTROYED;
    }
    if (error.Failed()) {
      NS_WARNING("Selection::SetBaseAndExtentInLimiter() failed");
      return error.StealNSResult();
    }
  }

  AutoSelectionRestorer restoreSelectionLater(*this);

  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  {
    // TODO: We don't need AutoTransactionsConserveSelection here in the normal
    //       cases, but removing this may cause the behavior with the legacy
    //       mutation event listeners.  We should try to delete this in a bug.
    AutoTransactionsConserveSelection dontChangeMySelection(*this);

    {
      AutoRangeArray extendedSelectionRanges(SelectionRef());
      extendedSelectionRanges
          .ExtendRangesToWrapLinesToHandleBlockLevelEditAction(
              EditSubAction::eCreateOrChangeList, aEditingHost);
      Result<EditorDOMPoint, nsresult> splitResult =
          extendedSelectionRanges
              .SplitTextNodesAtEndBoundariesAndParentInlineElementsAtBoundaries(
                  *this);
      if (MOZ_UNLIKELY(splitResult.isErr())) {
        NS_WARNING(
            "AutoRangeArray::"
            "SplitTextNodesAtEndBoundariesAndParentInlineElementsAtBoundaries()"
            " failed");
        return splitResult.unwrapErr();
      }
      nsresult rv = extendedSelectionRanges.CollectEditTargetNodes(
          *this, arrayOfContents, EditSubAction::eCreateOrChangeList,
          AutoRangeArray::CollectNonEditableNodes::No);
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "AutoRangeArray::CollectEditTargetNodes(EditSubAction::"
            "eCreateOrChangeList, CollectNonEditableNodes::No) failed");
        return rv;
      }
    }

    const Result<EditorDOMPoint, nsresult> splitAtBRElementsResult =
        MaybeSplitElementsAtEveryBRElement(arrayOfContents,
                                           EditSubAction::eCreateOrChangeList);
    if (MOZ_UNLIKELY(splitAtBRElementsResult.isErr())) {
      NS_WARNING(
          "HTMLEditor::MaybeSplitElementsAtEveryBRElement(EditSubAction::"
          "eCreateOrChangeList) failed");
      return splitAtBRElementsResult.inspectErr();
    }
  }

  // Remove all non-editable nodes.  Leave them be.
  // XXX CollectEditTargetNodes() should return only editable contents when it's
  //     called with CollectNonEditableNodes::No, but checking it here, looks
  //     like just wasting the runtime cost.
  for (int32_t i = arrayOfContents.Length() - 1; i >= 0; i--) {
    OwningNonNull<nsIContent>& content = arrayOfContents[i];
    if (!EditorUtils::IsEditableContent(content, EditorType::HTML)) {
      arrayOfContents.RemoveElementAt(i);
    }
  }

  // Only act on lists or list items in the array
  for (auto& content : arrayOfContents) {
    // here's where we actually figure out what to do
    if (HTMLEditUtils::IsListItem(content)) {
      // unlist this listitem
      nsresult rv = LiftUpListItemElement(MOZ_KnownLive(*content->AsElement()),
                                          LiftUpFromAllParentListElements::Yes);
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "HTMLEditor::LiftUpListItemElement(LiftUpFromAllParentListElements:"
            ":Yes) failed");
        return rv;
      }
      continue;
    }
    if (HTMLEditUtils::IsAnyListElement(content)) {
      // node is a list, move list items out
      nsresult rv =
          DestroyListStructureRecursively(MOZ_KnownLive(*content->AsElement()));
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::DestroyListStructureRecursively() failed");
        return rv;
      }
      continue;
    }
  }
  return NS_OK;
}

Result<RefPtr<Element>, nsresult>
HTMLEditor::FormatBlockContainerWithTransaction(
    AutoRangeArray& aSelectionRanges, nsAtom& blockType,
    const Element& aEditingHost) {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  // XXX Why do we do this only when there is only one selection range?
  if (!aSelectionRanges.IsCollapsed() &&
      aSelectionRanges.Ranges().Length() == 1u) {
    Result<EditorRawDOMRange, nsresult> extendedRange =
        GetRangeExtendedToHardLineEdgesForBlockEditAction(
            aSelectionRanges.FirstRangeRef(), aEditingHost);
    if (MOZ_UNLIKELY(extendedRange.isErr())) {
      NS_WARNING(
          "HTMLEditor::GetRangeExtendedToHardLineEdgesForBlockEditAction() "
          "failed");
      return extendedRange.propagateErr();
    }
    // Note that end point may be prior to start point.  So, we
    // cannot use AutoRangeArray::SetStartAndEnd() here.
    if (NS_FAILED(aSelectionRanges.SetBaseAndExtent(
            extendedRange.inspect().StartRef(),
            extendedRange.inspect().EndRef()))) {
      NS_WARNING("AutoRangeArray::SetBaseAndExtent() failed");
      return Err(NS_ERROR_FAILURE);
    }
  }

  MOZ_ALWAYS_TRUE(aSelectionRanges.SaveAndTrackRanges(*this));

  // TODO: We don't need AutoTransactionsConserveSelection here in the normal
  //       cases, but removing this may cause the behavior with the legacy
  //       mutation event listeners.  We should try to delete this in a bug.
  AutoTransactionsConserveSelection dontChangeMySelection(*this);

  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  aSelectionRanges.ExtendRangesToWrapLinesToHandleBlockLevelEditAction(
      EditSubAction::eCreateOrRemoveBlock, aEditingHost);
  Result<EditorDOMPoint, nsresult> splitResult =
      aSelectionRanges
          .SplitTextNodesAtEndBoundariesAndParentInlineElementsAtBoundaries(
              *this);
  if (MOZ_UNLIKELY(splitResult.isErr())) {
    NS_WARNING(
        "AutoRangeArray::"
        "SplitTextNodesAtEndBoundariesAndParentInlineElementsAtBoundaries() "
        "failed");
    return splitResult.propagateErr();
  }
  nsresult rv = aSelectionRanges.CollectEditTargetNodes(
      *this, arrayOfContents, EditSubAction::eCreateOrRemoveBlock,
      AutoRangeArray::CollectNonEditableNodes::Yes);
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "AutoRangeArray::CollectEditTargetNodes(EditSubAction::"
        "eCreateOrRemoveBlock, CollectNonEditableNodes::No) failed");
    return Err(rv);
  }

  Result<EditorDOMPoint, nsresult> splitAtBRElementsResult =
      MaybeSplitElementsAtEveryBRElement(arrayOfContents,
                                         EditSubAction::eCreateOrRemoveBlock);
  if (MOZ_UNLIKELY(splitAtBRElementsResult.isErr())) {
    NS_WARNING(
        "HTMLEditor::MaybeSplitElementsAtEveryBRElement(EditSubAction::"
        "eCreateOrRemoveBlock) failed");
    return splitAtBRElementsResult.propagateErr();
  }

  // If there is no visible and editable nodes in the edit targets, make an
  // empty block.
  // XXX Isn't this odd if there are only non-editable visible nodes?
  if (HTMLEditUtils::IsEmptyOneHardLine(arrayOfContents)) {
    if (NS_WARN_IF(aSelectionRanges.Ranges().IsEmpty())) {
      return Err(NS_ERROR_FAILURE);
    }

    auto pointToInsertBlock =
        aSelectionRanges.GetFirstRangeStartPoint<EditorDOMPoint>();
    if (&blockType == nsGkAtoms::normal || &blockType == nsGkAtoms::_empty) {
      if (!pointToInsertBlock.IsInContentNode()) {
        NS_WARNING(
            "HTMLEditor::FormatBlockContainerWithTransaction() couldn't find "
            "block parent because container of the point is not content");
        return Err(NS_ERROR_FAILURE);
      }
      // We are removing blocks (going to "body text")
      const RefPtr<Element> editableBlockElement =
          HTMLEditUtils::GetInclusiveAncestorElement(
              *pointToInsertBlock.ContainerAs<nsIContent>(),
              HTMLEditUtils::ClosestEditableBlockElement);
      if (!editableBlockElement) {
        NS_WARNING(
            "HTMLEditor::FormatBlockContainerWithTransaction() couldn't find "
            "block parent");
        return Err(NS_ERROR_FAILURE);
      }
      if (!HTMLEditUtils::IsFormatNode(editableBlockElement)) {
        return RefPtr<Element>();
      }

      // If the first editable node after selection is a br, consume it.
      // Otherwise it gets pushed into a following block after the split,
      // which is visually bad.
      if (nsCOMPtr<nsIContent> brContent = HTMLEditUtils::GetNextContent(
              pointToInsertBlock, {WalkTreeOption::IgnoreNonEditableNode},
              &aEditingHost)) {
        if (brContent && brContent->IsHTMLElement(nsGkAtoms::br)) {
          AutoEditorDOMPointChildInvalidator lockOffset(pointToInsertBlock);
          nsresult rv = DeleteNodeWithTransaction(*brContent);
          if (NS_FAILED(rv)) {
            NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
            return Err(rv);
          }
        }
      }
      // Do the splits!
      const SplitNodeResult splitNodeResult = SplitNodeDeepWithTransaction(
          *editableBlockElement, pointToInsertBlock,
          SplitAtEdges::eDoNotCreateEmptyContainer);
      if (splitNodeResult.isErr()) {
        NS_WARNING("HTMLEditor::SplitNodeDeepWithTransaction() failed");
        return Err(splitNodeResult.unwrapErr());
      }
      splitNodeResult.IgnoreCaretPointSuggestion();
      // Put a <br> element at the split point
      const CreateElementResult insertBRElementResult = InsertBRElement(
          WithTransaction::Yes, splitNodeResult.AtSplitPoint<EditorDOMPoint>());
      if (insertBRElementResult.isErr()) {
        NS_WARNING("HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
        return Err(insertBRElementResult.unwrapErr());
      }
      MOZ_ASSERT(insertBRElementResult.GetNewNode());
      aSelectionRanges.ClearSavedRanges();
      nsresult rv = aSelectionRanges.Collapse(
          EditorRawDOMPoint(insertBRElementResult.GetNewNode()));
      if (NS_FAILED(rv)) {
        NS_WARNING("AutoRangeArray::Collapse() failed");
        return Err(rv);
      }
      return RefPtr<Element>();
    }

    // We are making a block.  Consume a br, if needed.
    if (nsCOMPtr<nsIContent> maybeBRContent = HTMLEditUtils::GetNextContent(
            pointToInsertBlock,
            {WalkTreeOption::IgnoreNonEditableNode,
             WalkTreeOption::StopAtBlockBoundary},
            &aEditingHost)) {
      if (maybeBRContent->IsHTMLElement(nsGkAtoms::br)) {
        AutoEditorDOMPointChildInvalidator lockOffset(pointToInsertBlock);
        nsresult rv = DeleteNodeWithTransaction(*maybeBRContent);
        if (NS_FAILED(rv)) {
          NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
          return Err(rv);
        }
        // We don't need to act on this node any more
        arrayOfContents.RemoveElement(maybeBRContent);
      }
    }
    // Make sure we can put a block here.
    CreateElementResult createNewBlockElementResult =
        InsertElementWithSplittingAncestorsWithTransaction(
            blockType, pointToInsertBlock, BRElementNextToSplitPoint::Keep,
            aEditingHost);
    if (createNewBlockElementResult.isErr()) {
      NS_WARNING(
          nsPrintfCString(
              "HTMLEditor::InsertElementWithSplittingAncestorsWithTransaction("
              "%s) failed",
              nsAtomCString(&blockType).get())
              .get());
      return Err(createNewBlockElementResult.unwrapErr());
    }
    createNewBlockElementResult.IgnoreCaretPointSuggestion();
    MOZ_ASSERT(createNewBlockElementResult.GetNewNode());

    // Delete anything that was in the list of nodes
    while (!arrayOfContents.IsEmpty()) {
      OwningNonNull<nsIContent>& content = arrayOfContents[0];
      // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
      // keep it alive.
      nsresult rv = DeleteNodeWithTransaction(MOZ_KnownLive(*content));
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
        return Err(rv);
      }
      arrayOfContents.RemoveElementAt(0);
    }
    // Put selection in new block
    aSelectionRanges.ClearSavedRanges();
    nsresult rv = aSelectionRanges.Collapse(
        EditorRawDOMPoint(createNewBlockElementResult.GetNewNode(), 0u));
    if (NS_FAILED(rv)) {
      NS_WARNING("AutoRangeArray::Collapse() failed");
      return Err(rv);
    }
    return createNewBlockElementResult.UnwrapNewNode();
  }
  // Okay, now go through all the nodes and make the right kind of blocks, or
  // whatever is appropriate.
  // Note: blockquote is handled a little differently.
  if (&blockType == nsGkAtoms::blockquote) {
    CreateElementResult wrapContentsInBlockquoteElementsResult =
        WrapContentsInBlockquoteElementsWithTransaction(arrayOfContents,
                                                        aEditingHost);
    if (wrapContentsInBlockquoteElementsResult.isErr()) {
      NS_WARNING(
          "HTMLEditor::WrapContentsInBlockquoteElementsWithTransaction() "
          "failed");
      return Err(wrapContentsInBlockquoteElementsResult.unwrapErr());
    }
    wrapContentsInBlockquoteElementsResult.IgnoreCaretPointSuggestion();
    return wrapContentsInBlockquoteElementsResult.UnwrapNewNode();
  }
  if (&blockType == nsGkAtoms::normal || &blockType == nsGkAtoms::_empty) {
    Result<EditorDOMPoint, nsresult> removeBlockContainerElementsResult =
        RemoveBlockContainerElementsWithTransaction(arrayOfContents);
    if (MOZ_UNLIKELY(removeBlockContainerElementsResult.isErr())) {
      NS_WARNING(
          "HTMLEditor::RemoveBlockContainerElementsWithTransaction() failed");
      removeBlockContainerElementsResult.unwrapErr();
    }
    return RefPtr<Element>();
  }
  CreateElementResult wrapContentsInBlockElementResult =
      CreateOrChangeBlockContainerElement(arrayOfContents, blockType,
                                          aEditingHost);
  if (MOZ_UNLIKELY(wrapContentsInBlockElementResult.isErr())) {
    NS_WARNING("HTMLEditor::CreateOrChangeBlockContainerElement() failed");
    return Err(wrapContentsInBlockElementResult.unwrapErr());
  }
  wrapContentsInBlockElementResult.IgnoreCaretPointSuggestion();
  return wrapContentsInBlockElementResult.UnwrapNewNode();
}

nsresult HTMLEditor::MaybeInsertPaddingBRElementForEmptyLastLineAtSelection() {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(!IsSelectionRangeContainerNotContent());

  if (!SelectionRef().IsCollapsed()) {
    return NS_OK;
  }

  const nsRange* firstRange = SelectionRef().GetRangeAt(0);
  if (NS_WARN_IF(!firstRange)) {
    return NS_ERROR_FAILURE;
  }
  const RangeBoundary& atStartOfSelection = firstRange->StartRef();
  if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
    return NS_ERROR_FAILURE;
  }
  if (!atStartOfSelection.Container()->IsElement()) {
    return NS_OK;
  }
  OwningNonNull<Element> startContainerElement =
      *atStartOfSelection.Container()->AsElement();
  nsresult rv =
      InsertPaddingBRElementForEmptyLastLineIfNeeded(startContainerElement);
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::InsertPaddingBRElementForEmptyLastLineIfNeeded() failed");
  return rv;
}

EditActionResult HTMLEditor::IndentAsSubAction(const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  AutoPlaceholderBatch treatAsOneTransaction(
      *this, ScrollSelectionIntoView::Yes, __FUNCTION__);
  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this, EditSubAction::eIndent, nsIEditor::eNext, ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return EditActionResult(ignoredError.StealNSResult());
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Some selection containers are not content node, but ignored");
    return EditActionIgnored();
  }

  result |= HandleIndentAtSelection(aEditingHost);
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::HandleIndentAtSelection() failed");
    return result;
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Mutation event listener might have changed selection");
    return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  // TODO: Investigate when we need to put a `<br>` element after indenting
  //       ranges.  Then, we could stop calling this here, or maybe we need to
  //       do it while moving content nodes.
  nsresult rv = MaybeInsertPaddingBRElementForEmptyLastLineAtSelection();
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "MaybeInsertPaddingBRElementForEmptyLastLineAtSelection() failed");
  return result.SetResult(rv);
}

Result<EditorDOMPoint, nsresult> HTMLEditor::IndentListChildWithTransaction(
    RefPtr<Element>* aSubListElement, const EditorDOMPoint& aPointInListElement,
    nsIContent& aContentMovingToSubList, const Element& aEditingHost) {
  MOZ_ASSERT(
      HTMLEditUtils::IsAnyListElement(aPointInListElement.GetContainer()),
      "unexpected container");
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  // some logic for putting list items into nested lists...

  // If aContentMovingToSubList is followed by a sub-list element whose tag is
  // same as the parent list element's tag, we can move it to start of the
  // sub-list.
  if (nsIContent* nextEditableSibling = HTMLEditUtils::GetNextSibling(
          aContentMovingToSubList, {WalkTreeOption::IgnoreWhiteSpaceOnlyText,
                                    WalkTreeOption::IgnoreNonEditableNode})) {
    if (HTMLEditUtils::IsAnyListElement(nextEditableSibling) &&
        aPointInListElement.GetContainer()->NodeInfo()->NameAtom() ==
            nextEditableSibling->NodeInfo()->NameAtom() &&
        aPointInListElement.GetContainer()->NodeInfo()->NamespaceID() ==
            nextEditableSibling->NodeInfo()->NamespaceID()) {
      MoveNodeResult moveListElementResult = MoveNodeWithTransaction(
          aContentMovingToSubList, EditorDOMPoint(nextEditableSibling, 0u));
      if (moveListElementResult.isErr()) {
        NS_WARNING("HTMLEditor::MoveNodeWithTransaction() failed");
        return Err(moveListElementResult.unwrapErr());
      }
      return moveListElementResult.UnwrapCaretPoint();
    }
  }

  // If aContentMovingToSubList follows a sub-list element whose tag is same
  // as the parent list element's tag, we can move it to end of the sub-list.
  if (nsCOMPtr<nsIContent> previousEditableSibling =
          HTMLEditUtils::GetPreviousSibling(
              aContentMovingToSubList,
              {WalkTreeOption::IgnoreWhiteSpaceOnlyText,
               WalkTreeOption::IgnoreNonEditableNode})) {
    if (HTMLEditUtils::IsAnyListElement(previousEditableSibling) &&
        aPointInListElement.GetContainer()->NodeInfo()->NameAtom() ==
            previousEditableSibling->NodeInfo()->NameAtom() &&
        aPointInListElement.GetContainer()->NodeInfo()->NamespaceID() ==
            previousEditableSibling->NodeInfo()->NamespaceID()) {
      MoveNodeResult moveListElementResult = MoveNodeToEndWithTransaction(
          aContentMovingToSubList, *previousEditableSibling);
      if (moveListElementResult.isErr()) {
        NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
        return Err(moveListElementResult.unwrapErr());
      }
      return moveListElementResult.UnwrapCaretPoint();
    }
  }

  // If aContentMovingToSubList does not follow aSubListElement, we need
  // to create new sub-list element.
  EditorDOMPoint pointToPutCaret;
  nsIContent* previousEditableSibling =
      *aSubListElement ? HTMLEditUtils::GetPreviousSibling(
                             aContentMovingToSubList,
                             {WalkTreeOption::IgnoreWhiteSpaceOnlyText,
                              WalkTreeOption::IgnoreNonEditableNode})
                       : nullptr;
  if (!*aSubListElement || (previousEditableSibling &&
                            previousEditableSibling != *aSubListElement)) {
    nsAtom* containerName =
        aPointInListElement.GetContainer()->NodeInfo()->NameAtom();
    // Create a new nested list of correct type.
    CreateElementResult createNewListElementResult =
        InsertElementWithSplittingAncestorsWithTransaction(
            MOZ_KnownLive(*containerName), aPointInListElement,
            BRElementNextToSplitPoint::Keep, aEditingHost);
    if (createNewListElementResult.isErr()) {
      NS_WARNING(
          nsPrintfCString(
              "HTMLEditor::InsertElementWithSplittingAncestorsWithTransaction("
              "%s) failed",
              nsAtomCString(containerName).get())
              .get());
      return Err(createNewListElementResult.unwrapErr());
    }
    MOZ_ASSERT(createNewListElementResult.GetNewNode());
    pointToPutCaret = createNewListElementResult.UnwrapCaretPoint();
    *aSubListElement = createNewListElementResult.UnwrapNewNode();
  }

  // Finally, we should move aContentMovingToSubList into aSubListElement.
  const RefPtr<Element> subListElement = *aSubListElement;
  MoveNodeResult moveNodeResult =
      MoveNodeToEndWithTransaction(aContentMovingToSubList, *subListElement);
  if (moveNodeResult.isErr()) {
    NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
    return Err(moveNodeResult.unwrapErr());
  }
  if (moveNodeResult.HasCaretPointSuggestion()) {
    pointToPutCaret = moveNodeResult.UnwrapCaretPoint();
  }
  return pointToPutCaret;
}

EditActionResult HTMLEditor::HandleIndentAtSelection(
    const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(!IsSelectionRangeContainerNotContent());

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (NS_SUCCEEDED(rv) && SelectionRef().IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterInvisibleBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterInvisibleBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  AutoRangeArray selectionRanges(SelectionRef());

  if (!selectionRanges.IsInContent()) {
    NS_WARNING("Mutation event listener might have changed the selection");
    return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  if (IsCSSEnabled()) {
    nsresult rv = HandleCSSIndentAroundRanges(selectionRanges, aEditingHost);
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::HandleCSSIndentAroundRanges() failed");
      return EditActionHandled(rv);
    }
  } else {
    nsresult rv = HandleHTMLIndentAroundRanges(selectionRanges, aEditingHost);
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::HandleHTMLIndentAroundRanges() failed");
      return EditActionHandled(rv);
    }
  }
  rv = selectionRanges.ApplyTo(SelectionRef());
  if (MOZ_UNLIKELY(Destroyed())) {
    NS_WARNING("AutoRangeArray::ApplyTo() caused destroying the editor");
    return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::ApplyTo() failed");
  return EditActionHandled(rv);
}

nsresult HTMLEditor::HandleCSSIndentAroundRanges(AutoRangeArray& aRanges,
                                                 const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(!aRanges.Ranges().IsEmpty());
  MOZ_ASSERT(aRanges.IsInContent());

  if (aRanges.Ranges().IsEmpty()) {
    NS_WARNING("There is no selection range");
    return NS_ERROR_FAILURE;
  }

  // XXX Why do we do this only when there is only one selection range?
  if (!aRanges.IsCollapsed() && aRanges.Ranges().Length() == 1u) {
    Result<EditorRawDOMRange, nsresult> extendedRange =
        GetRangeExtendedToHardLineEdgesForBlockEditAction(
            aRanges.FirstRangeRef(), aEditingHost);
    if (MOZ_UNLIKELY(extendedRange.isErr())) {
      NS_WARNING(
          "HTMLEditor::GetRangeExtendedToHardLineEdgesForBlockEditAction() "
          "failed");
      return extendedRange.unwrapErr();
    }
    // Note that end point may be prior to start point.  So, we
    // cannot use SetStartAndEnd() here.
    nsresult rv = aRanges.SetBaseAndExtent(extendedRange.inspect().StartRef(),
                                           extendedRange.inspect().EndRef());
    if (NS_FAILED(rv)) {
      NS_WARNING("AutoRangeArray::SetBaseAndExtent() failed");
      return rv;
    }
  }

  if (NS_WARN_IF(!aRanges.SaveAndTrackRanges(*this))) {
    return NS_ERROR_FAILURE;
  }

  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;

  // short circuit: detect case of collapsed selection inside an <li>.
  // just sublist that <li>.  This prevents bug 97797.

  if (aRanges.IsCollapsed()) {
    const auto atCaret = aRanges.GetFirstRangeStartPoint<EditorRawDOMPoint>();
    if (NS_WARN_IF(!atCaret.IsSet())) {
      return NS_ERROR_FAILURE;
    }
    MOZ_ASSERT(atCaret.IsInContentNode());
    Element* const editableBlockElement =
        HTMLEditUtils::GetInclusiveAncestorElement(
            *atCaret.ContainerAs<nsIContent>(),
            HTMLEditUtils::ClosestEditableBlockElement);
    if (editableBlockElement &&
        HTMLEditUtils::IsListItem(editableBlockElement)) {
      arrayOfContents.AppendElement(*editableBlockElement);
    }
  }

  EditorDOMPoint pointToPutCaret;
  if (arrayOfContents.IsEmpty()) {
    {
      AutoRangeArray extendedRanges(aRanges);
      extendedRanges.ExtendRangesToWrapLinesToHandleBlockLevelEditAction(
          EditSubAction::eIndent, aEditingHost);
      Result<EditorDOMPoint, nsresult> splitResult =
          extendedRanges
              .SplitTextNodesAtEndBoundariesAndParentInlineElementsAtBoundaries(
                  *this);
      if (MOZ_UNLIKELY(splitResult.isErr())) {
        NS_WARNING(
            "AutoRangeArray::"
            "SplitTextNodesAtEndBoundariesAndParentInlineElementsAtBoundaries()"
            " failed");
        return splitResult.unwrapErr();
      }
      if (splitResult.inspect().IsSet()) {
        pointToPutCaret = splitResult.unwrap();
      }
      nsresult rv = extendedRanges.CollectEditTargetNodes(
          *this, arrayOfContents, EditSubAction::eIndent,
          AutoRangeArray::CollectNonEditableNodes::Yes);
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "AutoRangeArray::CollectEditTargetNodes(EditSubAction::eIndent, "
            "CollectNonEditableNodes::Yes) failed");
        return rv;
      }
    }
    Result<EditorDOMPoint, nsresult> splitAtBRElementsResult =
        MaybeSplitElementsAtEveryBRElement(arrayOfContents,
                                           EditSubAction::eIndent);
    if (MOZ_UNLIKELY(splitAtBRElementsResult.isErr())) {
      NS_WARNING(
          "HTMLEditor::MaybeSplitElementsAtEveryBRElement(EditSubAction::"
          "eIndent) failed");
      return splitAtBRElementsResult.inspectErr();
    }
    if (splitAtBRElementsResult.inspect().IsSet()) {
      pointToPutCaret = splitAtBRElementsResult.unwrap();
    }
  }

  // If there is no visible and editable nodes in the edit targets, make an
  // empty block.
  // XXX Isn't this odd if there are only non-editable visible nodes?
  if (HTMLEditUtils::IsEmptyOneHardLine(arrayOfContents)) {
    const EditorDOMPoint pointToInsertDivElement =
        pointToPutCaret.IsSet()
            ? std::move(pointToPutCaret)
            : aRanges.GetFirstRangeStartPoint<EditorDOMPoint>();
    if (NS_WARN_IF(!pointToPutCaret.IsSet())) {
      return NS_ERROR_FAILURE;
    }

    // make sure we can put a block here
    CreateElementResult createNewDivElementResult =
        InsertElementWithSplittingAncestorsWithTransaction(
            *nsGkAtoms::div, pointToInsertDivElement,
            BRElementNextToSplitPoint::Keep, aEditingHost);
    if (createNewDivElementResult.isErr()) {
      NS_WARNING(
          "HTMLEditor::InsertElementWithSplittingAncestorsWithTransaction("
          "nsGkAtoms::div) failed");
      return createNewDivElementResult.unwrapErr();
    }
    // We'll collapse ranges below, so we don't need to touch the ranges here.
    createNewDivElementResult.IgnoreCaretPointSuggestion();
    const RefPtr<Element> newDivElement =
        createNewDivElementResult.UnwrapNewNode();
    MOZ_ASSERT(newDivElement);
    const Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
        ChangeMarginStart(*newDivElement, ChangeMargin::Increase, aEditingHost);
    if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
      if (NS_WARN_IF(pointToPutCaretOrError.inspectErr() ==
                     NS_ERROR_EDITOR_DESTROYED)) {
        return NS_ERROR_EDITOR_DESTROYED;
      }
      NS_WARNING(
          "HTMLEditor::ChangeMarginStart(ChangeMargin::Increase) failed, but "
          "ignored");
    }
    // delete anything that was in the list of nodes
    // XXX We don't need to remove the nodes from the array for performance.
    for (const OwningNonNull<nsIContent>& content : arrayOfContents) {
      // MOZ_KnownLive(content) due to bug 1622253
      nsresult rv = DeleteNodeWithTransaction(MOZ_KnownLive(content));
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
        return rv;
      }
    }
    aRanges.ClearSavedRanges();
    nsresult rv = aRanges.Collapse(EditorDOMPoint(newDivElement, 0u));
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::Collapse() failed");
    return rv;
  }

  RefPtr<Element> latestNewBlockElement;
  auto RestoreSavedRangesAndCollapseInLatestBlockElementIfOutside =
      [&]() -> nsresult {
    MOZ_ASSERT(aRanges.HasSavedRanges());
    aRanges.RestoreFromSavedRanges();

    if (!latestNewBlockElement || !aRanges.IsCollapsed() ||
        aRanges.Ranges().IsEmpty()) {
      return NS_OK;
    }

    const auto firstRangeStartRawPoint =
        aRanges.GetFirstRangeStartPoint<EditorRawDOMPoint>();
    if (MOZ_UNLIKELY(!firstRangeStartRawPoint.IsSet())) {
      return NS_OK;
    }
    Result<EditorRawDOMPoint, nsresult> pointInNewBlockElementOrError =
        HTMLEditUtils::ComputePointToPutCaretInElementIfOutside<
            EditorRawDOMPoint>(*latestNewBlockElement, firstRangeStartRawPoint);
    if (MOZ_UNLIKELY(pointInNewBlockElementOrError.isErr())) {
      NS_WARNING(
          "HTMLEditUtils::ComputePointToPutCaretInElementIfOutside() failed, "
          "but ignored");
      return NS_OK;
    }
    if (!pointInNewBlockElementOrError.inspect().IsSet()) {
      return NS_OK;
    }
    return aRanges.Collapse(pointInNewBlockElementOrError.unwrap());
  };

  // Ok, now go through all the nodes and put them into sub-list element
  // elements and new <div> elements which have start margin.
  RefPtr<Element> subListElement, divElement;
  for (OwningNonNull<nsIContent>& content : arrayOfContents) {
    // Here's where we actually figure out what to do.
    EditorDOMPoint atContent(content);
    if (NS_WARN_IF(!atContent.IsSet())) {
      continue;
    }

    // Ignore all non-editable nodes.  Leave them be.
    // XXX We ignore non-editable nodes here, but not so in the above block.
    if (!EditorUtils::IsEditableContent(content, EditorType::HTML)) {
      continue;
    }

    if (HTMLEditUtils::IsAnyListElement(atContent.GetContainer())) {
      const RefPtr<Element> oldSubListElement = subListElement;
      // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
      // keep it alive.
      Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
          IndentListChildWithTransaction(&subListElement, atContent,
                                         MOZ_KnownLive(content), aEditingHost);
      if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
        NS_WARNING("HTMLEditor::IndentListChildWithTransaction() failed");
        return pointToPutCaretOrError.unwrapErr();
      }
      if (subListElement != oldSubListElement) {
        // New list element is created, so we should put caret into the new list
        // element.
        latestNewBlockElement = subListElement;
      }
      if (pointToPutCaretOrError.inspect().IsSet()) {
        pointToPutCaret = pointToPutCaretOrError.unwrap();
      }
      continue;
    }

    // Not a list item.

    if (HTMLEditUtils::IsBlockElement(content)) {
      Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
          ChangeMarginStart(MOZ_KnownLive(*content->AsElement()),
                            ChangeMargin::Increase, aEditingHost);
      if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
        if (MOZ_UNLIKELY(pointToPutCaretOrError.inspectErr() ==
                         NS_ERROR_EDITOR_DESTROYED)) {
          NS_WARNING(
              "HTMLEditor::ChangeMarginStart(ChangeMargin::Increase) failed");
          return NS_ERROR_EDITOR_DESTROYED;
        }
        NS_WARNING(
            "HTMLEditor::ChangeMarginStart(ChangeMargin::Increase) failed, but "
            "ignored");
      } else if (pointToPutCaretOrError.inspect().IsSet()) {
        pointToPutCaret = pointToPutCaretOrError.unwrap();
      }
      divElement = nullptr;
      continue;
    }

    if (!divElement) {
      // First, check that our element can contain a div.
      if (!HTMLEditUtils::CanNodeContain(*atContent.GetContainer(),
                                         *nsGkAtoms::div)) {
        // XXX This is odd, why do we stop indenting remaining content nodes?
        //     Perhaps, `continue` is better.
        nsresult rv =
            RestoreSavedRangesAndCollapseInLatestBlockElementIfOutside();
        NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                             "RestoreSavedRangesAndCollapseInLatestBlockElement"
                             "IfOutside() failed");
        return rv;
      }

      CreateElementResult createNewDivElementResult =
          InsertElementWithSplittingAncestorsWithTransaction(
              *nsGkAtoms::div, atContent, BRElementNextToSplitPoint::Keep,
              aEditingHost);
      if (createNewDivElementResult.isErr()) {
        NS_WARNING(
            "HTMLEditor::InsertElementWithSplittingAncestorsWithTransaction("
            "nsGkAtoms::div) failed");
        return createNewDivElementResult.unwrapErr();
      }
      pointToPutCaret = createNewDivElementResult.UnwrapCaretPoint();

      MOZ_ASSERT(createNewDivElementResult.GetNewNode());
      divElement = createNewDivElementResult.UnwrapNewNode();
      Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
          ChangeMarginStart(*divElement, ChangeMargin::Increase, aEditingHost);
      if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
        if (MOZ_UNLIKELY(pointToPutCaretOrError.inspectErr() ==
                         NS_ERROR_EDITOR_DESTROYED)) {
          NS_WARNING(
              "HTMLEditor::ChangeMarginStart(ChangeMargin::Increase) failed");
          return NS_ERROR_EDITOR_DESTROYED;
        }
        NS_WARNING(
            "HTMLEditor::ChangeMarginStart(ChangeMargin::Increase) failed, but "
            "ignored");
      } else if (AllowsTransactionsToChangeSelection() &&
                 pointToPutCaretOrError.inspect().IsSet()) {
        pointToPutCaret = pointToPutCaretOrError.unwrap();
      }

      latestNewBlockElement = divElement;
    }

    // Move the content into the <div> which has start margin.
    // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
    // keep it alive.
    MoveNodeResult moveNodeResult =
        MoveNodeToEndWithTransaction(MOZ_KnownLive(content), *divElement);
    if (moveNodeResult.isErr()) {
      NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
      return moveNodeResult.unwrapErr();
    }
    if (moveNodeResult.HasCaretPointSuggestion()) {
      pointToPutCaret = moveNodeResult.UnwrapCaretPoint();
    }
  }

  nsresult rv = RestoreSavedRangesAndCollapseInLatestBlockElementIfOutside();
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "RestoreSavedRangesAndCollapseInLatestBlockElementIfOutside() failed");
  return rv;
}

nsresult HTMLEditor::HandleHTMLIndentAroundRanges(AutoRangeArray& aRanges,
                                                  const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(!aRanges.Ranges().IsEmpty());
  MOZ_ASSERT(aRanges.IsInContent());

  // XXX Why do we do this only when there is only one range?
  if (!aRanges.IsCollapsed() && aRanges.Ranges().Length() == 1u) {
    Result<EditorRawDOMRange, nsresult> extendedRange =
        GetRangeExtendedToHardLineEdgesForBlockEditAction(
            aRanges.FirstRangeRef(), aEditingHost);
    if (MOZ_UNLIKELY(extendedRange.isErr())) {
      NS_WARNING(
          "HTMLEditor::GetRangeExtendedToHardLineEdgesForBlockEditAction() "
          "failed");
      return extendedRange.unwrapErr();
    }
    // Note that end point may be prior to start point.  So, we cannot use
    // SetStartAndEnd() here.
    nsresult rv = aRanges.SetBaseAndExtent(extendedRange.inspect().StartRef(),
                                           extendedRange.inspect().EndRef());
    if (NS_FAILED(rv)) {
      NS_WARNING("AutoRangeArray::SetBaseAndExtent() failed");
      return rv;
    }
  }

  if (NS_WARN_IF(!aRanges.SaveAndTrackRanges(*this))) {
    return NS_ERROR_FAILURE;
  }

  EditorDOMPoint pointToPutCaret;

  // convert the selection ranges into "promoted" selection ranges:
  // this basically just expands the range to include the immediate
  // block parent, and then further expands to include any ancestors
  // whose children are all in the range

  // use these ranges to construct a list of nodes to act on.
  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  {
    AutoRangeArray extendedRanges(aRanges);
    extendedRanges.ExtendRangesToWrapLinesToHandleBlockLevelEditAction(
        EditSubAction::eIndent, aEditingHost);
    Result<EditorDOMPoint, nsresult> splitResult =
        extendedRanges
            .SplitTextNodesAtEndBoundariesAndParentInlineElementsAtBoundaries(
                *this);
    if (MOZ_UNLIKELY(splitResult.isErr())) {
      NS_WARNING(
          "AutoRangeArray::"
          "SplitTextNodesAtEndBoundariesAndParentInlineElementsAtBoundaries() "
          "failed");
      return splitResult.unwrapErr();
    }
    if (splitResult.inspect().IsSet()) {
      pointToPutCaret = splitResult.unwrap();
    }
    nsresult rv = extendedRanges.CollectEditTargetNodes(
        *this, arrayOfContents, EditSubAction::eIndent,
        AutoRangeArray::CollectNonEditableNodes::Yes);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "AutoRangeArray::CollectEditTargetNodes(EditSubAction::eIndent, "
          "CollectNonEditableNodes::Yes) failed");
      return rv;
    }
  }

  Result<EditorDOMPoint, nsresult> splitAtBRElementsResult =
      MaybeSplitElementsAtEveryBRElement(arrayOfContents,
                                         EditSubAction::eIndent);
  if (MOZ_UNLIKELY(splitAtBRElementsResult.isErr())) {
    NS_WARNING(
        "HTMLEditor::MaybeSplitElementsAtEveryBRElement(EditSubAction::eIndent)"
        " failed");
    return splitAtBRElementsResult.inspectErr();
  }
  if (splitAtBRElementsResult.inspect().IsSet()) {
    pointToPutCaret = splitAtBRElementsResult.unwrap();
  }

  // If there is no visible and editable nodes in the edit targets, make an
  // empty block.
  // XXX Isn't this odd if there are only non-editable visible nodes?
  if (HTMLEditUtils::IsEmptyOneHardLine(arrayOfContents)) {
    const EditorDOMPoint pointToInsertBlockquoteElement =
        pointToPutCaret.IsSet()
            ? std::move(pointToPutCaret)
            : EditorBase::GetFirstSelectionStartPoint<EditorDOMPoint>();
    if (NS_WARN_IF(!pointToInsertBlockquoteElement.IsSet())) {
      return NS_ERROR_FAILURE;
    }

    // Make sure we can put a block here.
    CreateElementResult createNewBlockquoteElementResult =
        InsertElementWithSplittingAncestorsWithTransaction(
            *nsGkAtoms::blockquote, pointToInsertBlockquoteElement,
            BRElementNextToSplitPoint::Keep, aEditingHost);
    if (createNewBlockquoteElementResult.isErr()) {
      NS_WARNING(
          "HTMLEditor::InsertElementWithSplittingAncestorsWithTransaction("
          "nsGkAtoms::blockquote) failed");
      return createNewBlockquoteElementResult.unwrapErr();
    }
    createNewBlockquoteElementResult.IgnoreCaretPointSuggestion();
    RefPtr<Element> newBlockquoteElement =
        createNewBlockquoteElementResult.UnwrapNewNode();
    MOZ_ASSERT(newBlockquoteElement);
    // delete anything that was in the list of nodes
    // XXX We don't need to remove the nodes from the array for performance.
    for (const OwningNonNull<nsIContent>& content : arrayOfContents) {
      // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
      // keep it alive.
      nsresult rv = DeleteNodeWithTransaction(MOZ_KnownLive(*content));
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
        return rv;
      }
    }
    aRanges.ClearSavedRanges();
    nsresult rv = aRanges.Collapse(EditorRawDOMPoint(newBlockquoteElement, 0u));
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "EditorBase::CollapseSelectionToStartOf() failed");
    return rv;
  }

  RefPtr<Element> latestNewBlockElement;
  auto RestoreSavedRangesAndCollapseInLatestBlockElementIfOutside =
      [&]() -> nsresult {
    MOZ_ASSERT(aRanges.HasSavedRanges());
    aRanges.RestoreFromSavedRanges();

    if (!latestNewBlockElement || !aRanges.IsCollapsed() ||
        aRanges.Ranges().IsEmpty()) {
      return NS_OK;
    }

    const auto firstRangeStartRawPoint =
        aRanges.GetFirstRangeStartPoint<EditorRawDOMPoint>();
    if (MOZ_UNLIKELY(!firstRangeStartRawPoint.IsSet())) {
      return NS_OK;
    }
    Result<EditorRawDOMPoint, nsresult> pointInNewBlockElementOrError =
        HTMLEditUtils::ComputePointToPutCaretInElementIfOutside<
            EditorRawDOMPoint>(*latestNewBlockElement, firstRangeStartRawPoint);
    if (MOZ_UNLIKELY(pointInNewBlockElementOrError.isErr())) {
      NS_WARNING(
          "HTMLEditUtils::ComputePointToPutCaretInElementIfOutside() failed, "
          "but ignored");
      return NS_OK;
    }
    if (!pointInNewBlockElementOrError.inspect().IsSet()) {
      return NS_OK;
    }
    return aRanges.Collapse(pointInNewBlockElementOrError.unwrap());
  };

  // Ok, now go through all the nodes and put them in a blockquote,
  // or whatever is appropriate.  Wohoo!
  RefPtr<Element> subListElement, blockquoteElement, indentedListItemElement;
  for (OwningNonNull<nsIContent>& content : arrayOfContents) {
    // Here's where we actually figure out what to do.
    EditorDOMPoint atContent(content);
    if (NS_WARN_IF(!atContent.IsSet())) {
      continue;
    }

    // Ignore all non-editable nodes.  Leave them be.
    // XXX We ignore non-editable nodes here, but not so in the above block.
    if (!EditorUtils::IsEditableContent(content, EditorType::HTML)) {
      continue;
    }

    if (HTMLEditUtils::IsAnyListElement(atContent.GetContainer())) {
      const RefPtr<Element> oldSubListElement = subListElement;
      // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
      // keep it alive.
      Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
          IndentListChildWithTransaction(&subListElement, atContent,
                                         MOZ_KnownLive(content), aEditingHost);
      if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
        NS_WARNING("HTMLEditor::IndentListChildWithTransaction() failed");
        return pointToPutCaretOrError.unwrapErr();
      }
      if (oldSubListElement != subListElement) {
        // New list element is created, so we should put caret into the new list
        // element.
        latestNewBlockElement = subListElement;
      }
      if (pointToPutCaretOrError.inspect().IsSet()) {
        pointToPutCaret = pointToPutCaretOrError.unwrap();
      }
      blockquoteElement = nullptr;
      continue;
    }

    // Not a list item, use blockquote?

    // if we are inside a list item, we don't want to blockquote, we want
    // to sublist the list item.  We may have several nodes listed in the
    // array of nodes to act on, that are in the same list item.  Since
    // we only want to indent that li once, we must keep track of the most
    // recent indented list item, and not indent it if we find another node
    // to act on that is still inside the same li.
    if (RefPtr<Element> listItem =
            HTMLEditUtils::GetClosestAncestorListItemElement(content,
                                                             &aEditingHost)) {
      if (indentedListItemElement == listItem) {
        // already indented this list item
        continue;
      }
      // check to see if subListElement is still appropriate.  Which it is if
      // content is still right after it in the same list.
      nsIContent* previousEditableSibling =
          subListElement
              ? HTMLEditUtils::GetPreviousSibling(
                    *listItem, {WalkTreeOption::IgnoreNonEditableNode})
              : nullptr;
      if (!subListElement || (previousEditableSibling &&
                              previousEditableSibling != subListElement)) {
        EditorDOMPoint atListItem(listItem);
        if (NS_WARN_IF(!listItem)) {
          return NS_ERROR_FAILURE;
        }
        nsAtom* containerName =
            atListItem.GetContainer()->NodeInfo()->NameAtom();
        // Create a new nested list of correct type.
        CreateElementResult createNewListElementResult =
            InsertElementWithSplittingAncestorsWithTransaction(
                MOZ_KnownLive(*containerName), atListItem,
                BRElementNextToSplitPoint::Keep, aEditingHost);
        if (createNewListElementResult.isErr()) {
          NS_WARNING(nsPrintfCString("HTMLEditor::"
                                     "InsertElementWithSplittingAncestorsWithTr"
                                     "ansaction(%s) failed",
                                     nsAtomCString(containerName).get())
                         .get());
          return createNewListElementResult.unwrapErr();
        }
        if (createNewListElementResult.HasCaretPointSuggestion()) {
          pointToPutCaret = createNewListElementResult.UnwrapCaretPoint();
        }
        MOZ_ASSERT(createNewListElementResult.GetNewNode());
        subListElement = createNewListElementResult.UnwrapNewNode();
      }

      MoveNodeResult moveListItemElementResult =
          MoveNodeToEndWithTransaction(*listItem, *subListElement);
      if (moveListItemElementResult.isErr()) {
        NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
        return moveListItemElementResult.unwrapErr();
      }
      if (moveListItemElementResult.HasCaretPointSuggestion()) {
        pointToPutCaret = moveListItemElementResult.UnwrapCaretPoint();
      }

      // Remember the list item element which we indented now for ignoring its
      // children to avoid using <blockquote> in it.
      indentedListItemElement = std::move(listItem);

      continue;
    }

    // need to make a blockquote to put things in if we haven't already,
    // or if this node doesn't go in blockquote we used earlier.
    // One reason it might not go in prio blockquote is if we are now
    // in a different table cell.
    if (blockquoteElement &&
        HTMLEditUtils::GetInclusiveAncestorAnyTableElement(
            *blockquoteElement) !=
            HTMLEditUtils::GetInclusiveAncestorAnyTableElement(content)) {
      blockquoteElement = nullptr;
    }

    if (!blockquoteElement) {
      // First, check that our element can contain a blockquote.
      if (!HTMLEditUtils::CanNodeContain(*atContent.GetContainer(),
                                         *nsGkAtoms::blockquote)) {
        // XXX This is odd, why do we stop indenting remaining content nodes?
        //     Perhaps, `continue` is better.
        nsresult rv =
            RestoreSavedRangesAndCollapseInLatestBlockElementIfOutside();
        NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                             "RestoreSavedRangesAndCollapseInLatestBlockElement"
                             "IfOutside() failed");
        return rv;
      }

      CreateElementResult createNewBlockquoteElementResult =
          InsertElementWithSplittingAncestorsWithTransaction(
              *nsGkAtoms::blockquote, atContent,
              BRElementNextToSplitPoint::Keep, aEditingHost);
      if (createNewBlockquoteElementResult.isErr()) {
        NS_WARNING(
            "HTMLEditor::InsertElementWithSplittingAncestorsWithTransaction("
            "nsGkAtoms::blockquote) failed");
        return createNewBlockquoteElementResult.unwrapErr();
      }
      if (createNewBlockquoteElementResult.HasCaretPointSuggestion()) {
        pointToPutCaret = createNewBlockquoteElementResult.UnwrapCaretPoint();
      }

      MOZ_ASSERT(createNewBlockquoteElementResult.GetNewNode());
      blockquoteElement = createNewBlockquoteElementResult.UnwrapNewNode();
      latestNewBlockElement = blockquoteElement;
    }

    // tuck the node into the end of the active blockquote
    // MOZ_KnownLive because 'arrayOfContents' is guaranteed to
    // keep it alive.
    MoveNodeResult moveNodeResult = MoveNodeToEndWithTransaction(
        MOZ_KnownLive(content), *blockquoteElement);
    if (moveNodeResult.isErr()) {
      NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
      return moveNodeResult.unwrapErr();
    }
    if (moveNodeResult.HasCaretPointSuggestion()) {
      pointToPutCaret = moveNodeResult.UnwrapCaretPoint();
    }
    subListElement = nullptr;
  }

  nsresult rv = RestoreSavedRangesAndCollapseInLatestBlockElementIfOutside();
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "RestoreSavedRangesAndCollapseInLatestBlockElementIfOutside() failed");
  return rv;
}

EditActionResult HTMLEditor::OutdentAsSubAction(const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  AutoPlaceholderBatch treatAsOneTransaction(
      *this, ScrollSelectionIntoView::Yes, __FUNCTION__);
  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this, EditSubAction::eOutdent, nsIEditor::eNext, ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return EditActionResult(ignoredError.StealNSResult());
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Some selection containers are not content node, but ignored");
    return EditActionIgnored();
  }

  result |= HandleOutdentAtSelection(aEditingHost);
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::HandleOutdentAtSelection() failed");
    return result;
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Mutation event listener might have changed the selection");
    return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  nsresult rv = MaybeInsertPaddingBRElementForEmptyLastLineAtSelection();
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::MaybeInsertPaddingBRElementForEmptyLastLineAtSelection() "
      "failed");
  return result.SetResult(rv);
}

EditActionResult HTMLEditor::HandleOutdentAtSelection(
    const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(!IsSelectionRangeContainerNotContent());

  // XXX Why do we do this only when there is only one selection range?
  if (!SelectionRef().IsCollapsed() && SelectionRef().RangeCount() == 1u) {
    Result<EditorRawDOMRange, nsresult> extendedRange =
        GetRangeExtendedToHardLineEdgesForBlockEditAction(
            SelectionRef().GetRangeAt(0u), aEditingHost);
    if (MOZ_UNLIKELY(extendedRange.isErr())) {
      NS_WARNING(
          "HTMLEditor::GetRangeExtendedToHardLineEdgesForBlockEditAction() "
          "failed");
      return EditActionResult(extendedRange.unwrapErr());
    }
    // Note that end point may be prior to start point.  So, we
    // cannot use Selection::SetStartAndEndInLimit() here.
    IgnoredErrorResult error;
    SelectionRef().SetBaseAndExtentInLimiter(
        extendedRange.inspect().StartRef().ToRawRangeBoundary(),
        extendedRange.inspect().EndRef().ToRawRangeBoundary(), error);
    if (NS_WARN_IF(Destroyed())) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    if (error.Failed()) {
      NS_WARNING("Selection::SetBaseAndExtentInLimiter() failed");
      return EditActionResult(error.StealNSResult());
    }
  }

  // HandleOutdentAtSelectionInternal() creates AutoSelectionRestorer.
  // Therefore, even if it returns NS_OK, the editor might have been destroyed
  // at restoring Selection.
  SplitRangeOffFromNodeResult outdentResult =
      HandleOutdentAtSelectionInternal(aEditingHost);
  if (NS_WARN_IF(Destroyed())) {
    return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
  }
  if (outdentResult.isErr()) {
    NS_WARNING("HTMLEditor::HandleOutdentAtSelectionInternal() failed");
    return EditActionHandled(outdentResult.unwrapErr());
  }

  // Make sure selection didn't stick to last piece of content in old bq (only
  // a problem for collapsed selections)
  if (!outdentResult.GetLeftContent() && !outdentResult.GetRightContent()) {
    return EditActionHandled();
  }

  if (!SelectionRef().IsCollapsed()) {
    return EditActionHandled();
  }

  // Push selection past end of left element of last split indented element.
  if (outdentResult.GetLeftContent()) {
    const nsRange* firstRange = SelectionRef().GetRangeAt(0);
    if (NS_WARN_IF(!firstRange)) {
      return EditActionHandled();
    }
    const RangeBoundary& atStartOfSelection = firstRange->StartRef();
    if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
      return EditActionHandled(NS_ERROR_FAILURE);
    }
    if (atStartOfSelection.Container() == outdentResult.GetLeftContent() ||
        EditorUtils::IsDescendantOf(*atStartOfSelection.Container(),
                                    *outdentResult.GetLeftContent())) {
      // Selection is inside the left node - push it past it.
      EditorRawDOMPoint afterRememberedLeftBQ(
          EditorRawDOMPoint::After(*outdentResult.GetLeftContent()));
      NS_WARNING_ASSERTION(
          afterRememberedLeftBQ.IsSet(),
          "Failed to set after remembered left blockquote element");
      nsresult rv = CollapseSelectionTo(afterRememberedLeftBQ);
      if (MOZ_UNLIKELY(rv == NS_ERROR_EDITOR_DESTROYED)) {
        NS_WARNING(
            "EditorBase::CollapseSelectionTo() caused destroying the editor");
        return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "EditorBase::CollapseSelectionTo() failed, but ignored");
    }
  }
  // And pull selection before beginning of right element of last split
  // indented element.
  if (outdentResult.GetRightContent()) {
    const nsRange* firstRange = SelectionRef().GetRangeAt(0);
    if (NS_WARN_IF(!firstRange)) {
      return EditActionHandled();
    }
    const RangeBoundary& atStartOfSelection = firstRange->StartRef();
    if (NS_WARN_IF(!atStartOfSelection.IsSet())) {
      return EditActionHandled(NS_ERROR_FAILURE);
    }
    if (atStartOfSelection.Container() == outdentResult.GetRightContent() ||
        EditorUtils::IsDescendantOf(*atStartOfSelection.Container(),
                                    *outdentResult.GetRightContent())) {
      // Selection is inside the right element - push it before it.
      EditorRawDOMPoint atRememberedRightBQ(outdentResult.GetRightContent());
      nsresult rv = CollapseSelectionTo(atRememberedRightBQ);
      if (MOZ_UNLIKELY(rv == NS_ERROR_EDITOR_DESTROYED)) {
        NS_WARNING(
            "EditorBase::CollapseSelectionTo() caused destroying the editor");
        return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "EditorBase::CollapseSelectionTo() failed, but ignored");
    }
  }
  return EditActionHandled();
}

SplitRangeOffFromNodeResult HTMLEditor::HandleOutdentAtSelectionInternal(
    const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  AutoSelectionRestorer restoreSelectionLater(*this);

  bool useCSS = IsCSSEnabled();

  // Convert the selection ranges into "promoted" selection ranges: this
  // basically just expands the range to include the immediate block parent,
  // and then further expands to include any ancestors whose children are all
  // in the range
  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  {
    AutoRangeArray extendedSelectionRanges(SelectionRef());
    extendedSelectionRanges.ExtendRangesToWrapLinesToHandleBlockLevelEditAction(
        EditSubAction::eOutdent, aEditingHost);
    nsresult rv = extendedSelectionRanges.CollectEditTargetNodes(
        *this, arrayOfContents, EditSubAction::eOutdent,
        AutoRangeArray::CollectNonEditableNodes::Yes);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "AutoRangeArray::CollectEditTargetNodes(EditSubAction::eOutdent, "
          "CollectNonEditableNodes::Yes) failed");
      return SplitRangeOffFromNodeResult(rv);
    }
    const Result<EditorDOMPoint, nsresult> splitAtBRElementsResult =
        MaybeSplitElementsAtEveryBRElement(arrayOfContents,
                                           EditSubAction::eOutdent);
    if (MOZ_UNLIKELY(splitAtBRElementsResult.isErr())) {
      NS_WARNING(
          "HTMLEditor::MaybeSplitElementsAtEveryBRElement(EditSubAction::"
          "eOutdent) failed");
      return SplitRangeOffFromNodeResult(splitAtBRElementsResult.inspectErr());
    }
    if (AllowsTransactionsToChangeSelection() &&
        splitAtBRElementsResult.inspect().IsSet()) {
      nsresult rv = CollapseSelectionTo(splitAtBRElementsResult.inspect());
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::CollapseSelectionTo() failed");
        return SplitRangeOffFromNodeResult(rv);
      }
    }
  }

  nsCOMPtr<nsIContent> leftContentOfLastOutdented;
  nsCOMPtr<nsIContent> middleContentOfLastOutdented;
  nsCOMPtr<nsIContent> rightContentOfLastOutdented;
  RefPtr<Element> indentedParentElement;
  nsCOMPtr<nsIContent> firstContentToBeOutdented, lastContentToBeOutdented;
  BlockIndentedWith indentedParentIndentedWith = BlockIndentedWith::HTML;
  for (OwningNonNull<nsIContent>& content : arrayOfContents) {
    // Here's where we actually figure out what to do
    EditorDOMPoint atContent(content);
    if (!atContent.IsSet()) {
      continue;
    }

    // If it's a `<blockquote>`, remove it to outdent its children.
    if (content->IsHTMLElement(nsGkAtoms::blockquote)) {
      // If we've already found an ancestor block element indented, we need to
      // split it and remove the block element first.
      if (indentedParentElement) {
        NS_WARNING_ASSERTION(indentedParentElement == content,
                             "Indented parent element is not the <blockquote>");
        SplitRangeOffFromNodeResult outdentResult = OutdentPartOfBlock(
            *indentedParentElement, *firstContentToBeOutdented,
            *lastContentToBeOutdented, indentedParentIndentedWith,
            aEditingHost);
        if (outdentResult.isErr()) {
          NS_WARNING("HTMLEditor::OutdentPartOfBlock() failed");
          return outdentResult;
        }
        leftContentOfLastOutdented = outdentResult.GetLeftContent();
        middleContentOfLastOutdented = outdentResult.GetMiddleContent();
        rightContentOfLastOutdented = outdentResult.GetRightContent();
        indentedParentElement = nullptr;
        firstContentToBeOutdented = nullptr;
        lastContentToBeOutdented = nullptr;
        indentedParentIndentedWith = BlockIndentedWith::HTML;
      }
      const Result<EditorDOMPoint, nsresult> unwrapBlockquoteElementResult =
          RemoveBlockContainerWithTransaction(
              MOZ_KnownLive(*content->AsElement()));
      if (MOZ_UNLIKELY(unwrapBlockquoteElementResult.isErr())) {
        NS_WARNING("HTMLEditor::RemoveBlockContainerWithTransaction() failed");
        return SplitRangeOffFromNodeResult(
            unwrapBlockquoteElementResult.inspectErr());
      }
      const EditorDOMPoint& pointToPutCaret =
          unwrapBlockquoteElementResult.inspect();
      if (AllowsTransactionsToChangeSelection() && pointToPutCaret.IsSet()) {
        nsresult rv = CollapseSelectionTo(pointToPutCaret);
        if (NS_FAILED(rv)) {
          NS_WARNING("EditorBase::CollapseSelectionTo() failed");
          return SplitRangeOffFromNodeResult(rv);
        }
      }
      continue;
    }

    // If we're using CSS and the node is a block element, check its start
    // margin whether it's indented with CSS.
    if (useCSS && HTMLEditUtils::IsBlockElement(content)) {
      nsStaticAtom& marginProperty =
          MarginPropertyAtomForIndent(MOZ_KnownLive(content));
      if (NS_WARN_IF(Destroyed())) {
        return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
      }
      nsAutoString value;
      DebugOnly<nsresult> rvIgnored =
          CSSEditUtils::GetSpecifiedProperty(content, marginProperty, value);
      if (NS_WARN_IF(Destroyed())) {
        return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rvIgnored),
          "CSSEditUtils::GetSpecifiedProperty() failed, but ignored");
      float startMargin = 0;
      RefPtr<nsAtom> unit;
      CSSEditUtils::ParseLength(value, &startMargin, getter_AddRefs(unit));
      // If indented with CSS, we should decrease the start margin.
      if (startMargin > 0) {
        const Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
            ChangeMarginStart(MOZ_KnownLive(*content->AsElement()),
                              ChangeMargin::Decrease, aEditingHost);
        if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
          if (NS_WARN_IF(pointToPutCaretOrError.inspectErr() ==
                         NS_ERROR_EDITOR_DESTROYED)) {
            return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
          }
          NS_WARNING(
              "HTMLEditor::ChangeMarginStart(ChangeMargin::Decrease) failed, "
              "but ignored");
        } else if (AllowsTransactionsToChangeSelection() &&
                   pointToPutCaretOrError.inspect().IsSet()) {
          nsresult rv = CollapseSelectionTo(pointToPutCaretOrError.inspect());
          if (NS_FAILED(rv)) {
            NS_WARNING("EditorBase::CollapseSelectionTo() failed");
            return SplitRangeOffFromNodeResult(rv);
          }
        }
        continue;
      }
    }

    // If it's a list item, we should treat as that it "indents" its children.
    if (HTMLEditUtils::IsListItem(content)) {
      // If it is a list item, that means we are not outdenting whole list.
      // XXX I don't understand this sentence...  We may meet parent list
      //     element, no?
      if (indentedParentElement) {
        SplitRangeOffFromNodeResult outdentResult = OutdentPartOfBlock(
            *indentedParentElement, *firstContentToBeOutdented,
            *lastContentToBeOutdented, indentedParentIndentedWith,
            aEditingHost);
        if (outdentResult.isErr()) {
          NS_WARNING("HTMLEditor::OutdentPartOfBlock() failed");
          return outdentResult;
        }
        leftContentOfLastOutdented = outdentResult.GetLeftContent();
        middleContentOfLastOutdented = outdentResult.GetMiddleContent();
        rightContentOfLastOutdented = outdentResult.GetRightContent();
        indentedParentElement = nullptr;
        firstContentToBeOutdented = nullptr;
        lastContentToBeOutdented = nullptr;
        indentedParentIndentedWith = BlockIndentedWith::HTML;
      }
      // XXX `content` could become different element since
      //     `OutdentPartOfBlock()` may run mutation event listeners.
      nsresult rv = LiftUpListItemElement(MOZ_KnownLive(*content->AsElement()),
                                          LiftUpFromAllParentListElements::No);
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "HTMLEditor::LiftUpListItemElement(LiftUpFromAllParentListElements:"
            ":No) failed");
        return SplitRangeOffFromNodeResult(rv);
      }
      continue;
    }

    // If we've found an ancestor block element which indents its children
    // and the current node is NOT a descendant of it, we should remove it to
    // outdent its children.  Otherwise, i.e., current node is a descendant of
    // it, we meet new node which should be outdented when the indented parent
    // is removed.
    if (indentedParentElement) {
      if (EditorUtils::IsDescendantOf(*content, *indentedParentElement)) {
        // Extend the range to be outdented at removing the
        // indentedParentElement.
        lastContentToBeOutdented = content;
        continue;
      }
      SplitRangeOffFromNodeResult outdentResult = OutdentPartOfBlock(
          *indentedParentElement, *firstContentToBeOutdented,
          *lastContentToBeOutdented, indentedParentIndentedWith, aEditingHost);
      if (outdentResult.isErr()) {
        NS_WARNING("HTMLEditor::OutdentPartOfBlock() failed");
        return outdentResult;
      }
      leftContentOfLastOutdented = outdentResult.GetLeftContent();
      middleContentOfLastOutdented = outdentResult.GetMiddleContent();
      rightContentOfLastOutdented = outdentResult.GetRightContent();
      indentedParentElement = nullptr;
      firstContentToBeOutdented = nullptr;
      lastContentToBeOutdented = nullptr;
      // curBlockIndentedWith = HTMLEditor::BlockIndentedWith::HTML;

      // Then, we need to look for next indentedParentElement.
    }

    indentedParentIndentedWith = BlockIndentedWith::HTML;
    for (nsCOMPtr<nsIContent> parentContent = content->GetParent();
         parentContent && !parentContent->IsHTMLElement(nsGkAtoms::body) &&
         parentContent != &aEditingHost &&
         (parentContent->IsHTMLElement(nsGkAtoms::table) ||
          !HTMLEditUtils::IsAnyTableElement(parentContent));
         parentContent = parentContent->GetParent()) {
      // If we reach a `<blockquote>` ancestor, it should be split at next
      // time at least for outdenting current node.
      if (parentContent->IsHTMLElement(nsGkAtoms::blockquote)) {
        indentedParentElement = parentContent->AsElement();
        firstContentToBeOutdented = content;
        lastContentToBeOutdented = content;
        break;
      }

      if (!useCSS) {
        continue;
      }

      nsCOMPtr<nsINode> grandParentNode = parentContent->GetParentNode();
      nsStaticAtom& marginProperty =
          MarginPropertyAtomForIndent(MOZ_KnownLive(content));
      if (NS_WARN_IF(Destroyed())) {
        return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
      }
      if (NS_WARN_IF(grandParentNode != parentContent->GetParentNode())) {
        return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
      }
      nsAutoString value;
      DebugOnly<nsresult> rvIgnored = CSSEditUtils::GetSpecifiedProperty(
          *parentContent, marginProperty, value);
      if (NS_WARN_IF(Destroyed())) {
        return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rvIgnored),
          "CSSEditUtils::GetSpecifiedProperty() failed, but ignored");
      // XXX Now, editing host may become different element.  If so, shouldn't
      //     we stop this handling?
      float startMargin;
      RefPtr<nsAtom> unit;
      CSSEditUtils::ParseLength(value, &startMargin, getter_AddRefs(unit));
      // If we reach a block element which indents its children with start
      // margin, we should remove it at next time.
      if (startMargin > 0 &&
          !(HTMLEditUtils::IsAnyListElement(atContent.GetContainer()) &&
            HTMLEditUtils::IsAnyListElement(content))) {
        indentedParentElement = parentContent->AsElement();
        firstContentToBeOutdented = content;
        lastContentToBeOutdented = content;
        indentedParentIndentedWith = BlockIndentedWith::CSS;
        break;
      }
    }

    if (indentedParentElement) {
      continue;
    }

    // If we don't have any block elements which indents current node and
    // both current node and its parent are list element, remove current
    // node to move all its children to the parent list.
    // XXX This is buggy.  When both lists' item types are different,
    //     we create invalid tree.  E.g., `<ul>` may have `<dd>` as its
    //     list item element.
    if (HTMLEditUtils::IsAnyListElement(atContent.GetContainer())) {
      if (!HTMLEditUtils::IsAnyListElement(content)) {
        continue;
      }
      // Just unwrap this sublist
      const Result<EditorDOMPoint, nsresult> unwrapSubListElementResult =
          RemoveBlockContainerWithTransaction(
              MOZ_KnownLive(*content->AsElement()));
      if (MOZ_UNLIKELY(unwrapSubListElementResult.isErr())) {
        NS_WARNING("HTMLEditor::RemoveBlockContainerWithTransaction() failed");
        return SplitRangeOffFromNodeResult(
            unwrapSubListElementResult.inspectErr());
      }
      const EditorDOMPoint& pointToPutCaret =
          unwrapSubListElementResult.inspect();
      if (!AllowsTransactionsToChangeSelection() || !pointToPutCaret.IsSet()) {
        continue;
      }
      nsresult rv = CollapseSelectionTo(pointToPutCaret);
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::CollapseSelectionTo() failed");
        return SplitRangeOffFromNodeResult(rv);
      }
      continue;
    }

    // If current content is a list element but its parent is not a list
    // element, move children to where it is and remove it from the tree.
    if (HTMLEditUtils::IsAnyListElement(content)) {
      // XXX If mutation event listener appends new children forever, this
      //     becomes an infinite loop so that we should set limitation from
      //     first child count.
      for (nsCOMPtr<nsIContent> lastChildContent = content->GetLastChild();
           lastChildContent; lastChildContent = content->GetLastChild()) {
        if (HTMLEditUtils::IsListItem(lastChildContent)) {
          nsresult rv = LiftUpListItemElement(
              MOZ_KnownLive(*lastChildContent->AsElement()),
              LiftUpFromAllParentListElements::No);
          if (NS_FAILED(rv)) {
            NS_WARNING(
                "HTMLEditor::LiftUpListItemElement("
                "LiftUpFromAllParentListElements::No) failed");
            return SplitRangeOffFromNodeResult(rv);
          }
          continue;
        }

        if (HTMLEditUtils::IsAnyListElement(lastChildContent)) {
          // We have an embedded list, so move it out from under the parent
          // list. Be sure to put it after the parent list because this
          // loop iterates backwards through the parent's list of children.
          EditorDOMPoint afterCurrentList(EditorDOMPoint::After(atContent));
          NS_WARNING_ASSERTION(
              afterCurrentList.IsSet(),
              "Failed to set it to after current list element");
          const MoveNodeResult moveListElementResult =
              MoveNodeWithTransaction(*lastChildContent, afterCurrentList);
          if (moveListElementResult.isErr()) {
            NS_WARNING("HTMLEditor::MoveNodeWithTransaction() failed");
            return SplitRangeOffFromNodeResult(
                moveListElementResult.unwrapErr());
          }
          nsresult rv = moveListElementResult.SuggestCaretPointTo(
              *this, {SuggestCaret::OnlyIfHasSuggestion,
                      SuggestCaret::OnlyIfTransactionsAllowedToDoIt,
                      SuggestCaret::AndIgnoreTrivialError});
          if (NS_FAILED(rv)) {
            NS_WARNING("MoveNodeResult::SuggestCaretPointTo() failed");
            return SplitRangeOffFromNodeResult(rv);
          }
          NS_WARNING_ASSERTION(
              rv != NS_SUCCESS_EDITOR_BUT_IGNORED_TRIVIAL_ERROR,
              "MoveNodeResult::SuggestCaretPointTo() failed, but ignored");
          continue;
        }

        // Delete any non-list items for now
        // XXX Chrome moves it from the list element.  We should follow it.
        nsresult rv = DeleteNodeWithTransaction(*lastChildContent);
        if (NS_FAILED(rv)) {
          NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
          return SplitRangeOffFromNodeResult(rv);
        }
      }
      // Delete the now-empty list
      const Result<EditorDOMPoint, nsresult> unwrapListElementResult =
          RemoveBlockContainerWithTransaction(
              MOZ_KnownLive(*content->AsElement()));
      if (MOZ_UNLIKELY(unwrapListElementResult.isErr())) {
        NS_WARNING("HTMLEditor::RemoveBlockContainerWithTransaction() failed");
        return SplitRangeOffFromNodeResult(
            unwrapListElementResult.inspectErr());
      }
      const EditorDOMPoint& pointToPutCaret = unwrapListElementResult.inspect();
      if (!AllowsTransactionsToChangeSelection() || !pointToPutCaret.IsSet()) {
        continue;
      }
      nsresult rv = CollapseSelectionTo(pointToPutCaret);
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::CollapseSelectionTo() failed");
        return SplitRangeOffFromNodeResult(rv);
      }
      continue;
    }

    if (useCSS) {
      if (RefPtr<Element> element = content->GetAsElementOrParentElement()) {
        const Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
            ChangeMarginStart(*element, ChangeMargin::Decrease, aEditingHost);
        if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
          if (NS_WARN_IF(pointToPutCaretOrError.inspectErr() ==
                         NS_ERROR_EDITOR_DESTROYED)) {
            return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
          }
          NS_WARNING(
              "HTMLEditor::ChangeMarginStart(ChangeMargin::Decrease) failed, "
              "but ignored");
        } else if (AllowsTransactionsToChangeSelection() &&
                   pointToPutCaretOrError.inspect().IsSet()) {
          nsresult rv = CollapseSelectionTo(pointToPutCaretOrError.inspect());
          if (NS_FAILED(rv)) {
            NS_WARNING("EditorBase::CollapseSelectionTo() failed");
            return SplitRangeOffFromNodeResult(rv);
          }
        }
      }
      continue;
    }
  }

  if (!indentedParentElement) {
    return SplitRangeOffFromNodeResult(leftContentOfLastOutdented,
                                       middleContentOfLastOutdented,
                                       rightContentOfLastOutdented);
  }

  // We have a <blockquote> we haven't finished handling.
  SplitRangeOffFromNodeResult outdentResult = OutdentPartOfBlock(
      *indentedParentElement, *firstContentToBeOutdented,
      *lastContentToBeOutdented, indentedParentIndentedWith, aEditingHost);
  NS_WARNING_ASSERTION(outdentResult.isOk(),
                       "HTMLEditor::OutdentPartOfBlock() failed");
  return outdentResult;
}

SplitRangeOffFromNodeResult
HTMLEditor::RemoveBlockContainerElementWithTransactionBetween(
    Element& aBlockContainerElement, nsIContent& aStartOfRange,
    nsIContent& aEndOfRange) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  EditorDOMPoint pointToPutCaret;
  SplitRangeOffFromNodeResult splitResult = SplitRangeOffFromBlock(
      aBlockContainerElement, aStartOfRange, aEndOfRange);
  if (splitResult.EditorDestroyed()) {
    NS_WARNING("HTMLEditor::SplitRangeOffFromBlock() failed");
    return splitResult;
  }
  if (splitResult.isOk()) {
    splitResult.MoveCaretPointTo(pointToPutCaret,
                                 {SuggestCaret::OnlyIfHasSuggestion});
  } else {
    NS_WARNING(
        "HTMLEditor::SplitRangeOffFromBlock() failed, but might be ignored");
  }
  Result<EditorDOMPoint, nsresult> unwrapBlockElementResult =
      RemoveBlockContainerWithTransaction(aBlockContainerElement);
  if (unwrapBlockElementResult.isErr()) {
    NS_WARNING("HTMLEditor::RemoveBlockContainerWithTransaction() failed");
    return SplitRangeOffFromNodeResult(unwrapBlockElementResult.inspectErr());
  }
  if (unwrapBlockElementResult.inspect().IsSet()) {
    pointToPutCaret = unwrapBlockElementResult.unwrap();
  }
  return SplitRangeOffFromNodeResult(splitResult.GetLeftContent(), nullptr,
                                     splitResult.GetRightContent(),
                                     std::move(pointToPutCaret));
}

SplitRangeOffFromNodeResult HTMLEditor::SplitRangeOffFromBlock(
    Element& aBlockElement, nsIContent& aStartOfMiddleElement,
    nsIContent& aEndOfMiddleElement) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  // aStartOfMiddleElement and aEndOfMiddleElement must be exclusive
  // descendants of aBlockElement.
  MOZ_ASSERT(EditorUtils::IsDescendantOf(aStartOfMiddleElement, aBlockElement));
  MOZ_ASSERT(EditorUtils::IsDescendantOf(aEndOfMiddleElement, aBlockElement));

  EditorDOMPoint pointToPutCaret;
  // Split at the start.
  SplitNodeResult splitAtStartResult = SplitNodeDeepWithTransaction(
      aBlockElement, EditorDOMPoint(&aStartOfMiddleElement),
      SplitAtEdges::eDoNotCreateEmptyContainer);
  if (splitAtStartResult.EditorDestroyed()) {
    NS_WARNING("HTMLEditor::SplitNodeDeepWithTransaction() failed (at left)");
    return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(
      splitAtStartResult.isOk(),
      "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
      "eDoNotCreateEmptyContainer) at start of middle element failed");
  splitAtStartResult.MoveCaretPointTo(pointToPutCaret,
                                      {SuggestCaret::OnlyIfHasSuggestion});

  // Split at after the end
  auto atAfterEnd = EditorDOMPoint::After(aEndOfMiddleElement);
  SplitNodeResult splitAtEndResult = SplitNodeDeepWithTransaction(
      aBlockElement, atAfterEnd, SplitAtEdges::eDoNotCreateEmptyContainer);
  if (splitAtEndResult.EditorDestroyed()) {
    NS_WARNING("HTMLEditor::SplitNodeDeepWithTransaction() failed (at right)");
    return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(
      splitAtEndResult.isOk(),
      "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
      "eDoNotCreateEmptyContainer) after end of middle element failed");
  splitAtEndResult.MoveCaretPointTo(pointToPutCaret,
                                    {SuggestCaret::OnlyIfHasSuggestion});

  if (splitAtStartResult.DidSplit() && splitAtEndResult.DidSplit()) {
    // Note that the middle node can be computed only with the latter split
    // result.
    return SplitRangeOffFromNodeResult(splitAtStartResult.GetPreviousContent(),
                                       splitAtEndResult.GetPreviousContent(),
                                       splitAtEndResult.GetNextContent(),
                                       std::move(pointToPutCaret));
  }
  if (splitAtStartResult.DidSplit()) {
    return SplitRangeOffFromNodeResult(splitAtStartResult.GetPreviousContent(),
                                       splitAtStartResult.GetNextContent(),
                                       nullptr, std::move(pointToPutCaret));
  }
  if (splitAtEndResult.DidSplit()) {
    return SplitRangeOffFromNodeResult(
        nullptr, splitAtEndResult.GetPreviousContent(),
        splitAtEndResult.GetNextContent(), std::move(pointToPutCaret));
  }
  return SplitRangeOffFromNodeResult(nullptr, &aBlockElement, nullptr,
                                     std::move(pointToPutCaret));
}

SplitRangeOffFromNodeResult HTMLEditor::OutdentPartOfBlock(
    Element& aBlockElement, nsIContent& aStartOfOutdent,
    nsIContent& aEndOfOutdent, BlockIndentedWith aBlockIndentedWith,
    const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  SplitRangeOffFromNodeResult splitResult =
      SplitRangeOffFromBlock(aBlockElement, aStartOfOutdent, aEndOfOutdent);
  if (splitResult.EditorDestroyed()) {
    NS_WARNING("HTMLEditor::SplitRangeOffFromBlock() failed");
    return SplitRangeOffFromNodeResult(NS_ERROR_EDITOR_DESTROYED);
  }

  if (!splitResult.GetMiddleContentAs<Element>()) {
    NS_WARNING(
        "HTMLEditor::SplitRangeOffFromBlock() didn't return middle content");
    splitResult.IgnoreCaretPointSuggestion();
    return SplitRangeOffFromNodeResult(NS_ERROR_FAILURE);
  }

  if (splitResult.isOk()) {
    nsresult rv = splitResult.SuggestCaretPointTo(
        *this, {SuggestCaret::OnlyIfHasSuggestion,
                SuggestCaret::OnlyIfTransactionsAllowedToDoIt,
                SuggestCaret::AndIgnoreTrivialError});
    if (NS_FAILED(rv)) {
      NS_WARNING("SplitRangeOffFromNodeResult::SuggestCaretPointTo() failed");
      return SplitRangeOffFromNodeResult(rv);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "SplitRangeOffFromNodeResult::SuggestCaretPointTo() "
                         "failed, but ignored");
  } else {
    NS_WARNING(
        "HTMLEditor::SplitRangeOffFromBlock() failed, but might be ignored");
  }

  if (aBlockIndentedWith == BlockIndentedWith::HTML) {
    // MOZ_KnownLive: perhaps, it does not work with template methods.
    Result<EditorDOMPoint, nsresult> unwrapBlockElementResult =
        RemoveBlockContainerWithTransaction(
            MOZ_KnownLive(*splitResult.GetMiddleContentAs<Element>()));
    if (MOZ_UNLIKELY(unwrapBlockElementResult.isErr())) {
      NS_WARNING("HTMLEditor::RemoveBlockContainerWithTransaction() failed");
      return SplitRangeOffFromNodeResult(unwrapBlockElementResult.inspectErr());
    }
    const EditorDOMPoint& pointToPutCaret = unwrapBlockElementResult.inspect();
    if (AllowsTransactionsToChangeSelection() && pointToPutCaret.IsSet()) {
      nsresult rv = CollapseSelectionTo(pointToPutCaret);
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::CollapseSelectionTo() failed");
        return SplitRangeOffFromNodeResult(rv);
      }
    }
    return SplitRangeOffFromNodeResult(splitResult.GetLeftContent(), nullptr,
                                       splitResult.GetRightContent());
  }

  if (!splitResult.GetMiddleContentAs<Element>()) {
    return splitResult;
  }

  // MOZ_KnownLive: perhaps, it does not work with template methods.
  const Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
      ChangeMarginStart(
          MOZ_KnownLive(*splitResult.GetMiddleContentAs<Element>()),
          ChangeMargin::Decrease, aEditingHost);
  if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
    NS_WARNING("HTMLEditor::ChangeMarginStart(ChangeMargin::Decrease) failed");
    return SplitRangeOffFromNodeResult(pointToPutCaretOrError.inspectErr());
  }
  if (AllowsTransactionsToChangeSelection() &&
      pointToPutCaretOrError.inspect().IsSet()) {
    nsresult rv = CollapseSelectionTo(pointToPutCaretOrError.inspect());
    if (NS_FAILED(rv)) {
      NS_WARNING("EditorBase::CollapseSelectionTo() failed");
      return SplitRangeOffFromNodeResult(rv);
    }
  }
  return splitResult;
}

CreateElementResult HTMLEditor::ChangeListElementType(Element& aListElement,
                                                      nsAtom& aNewListTag,
                                                      nsAtom& aNewListItemTag) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  EditorDOMPoint pointToPutCaret;

  AutoTArray<OwningNonNull<nsIContent>, 32> listElementChildren;
  HTMLEditor::GetChildNodesOf(aListElement, listElementChildren);

  for (const OwningNonNull<nsIContent>& childContent : listElementChildren) {
    if (!childContent->IsElement()) {
      continue;
    }
    Element* childElement = childContent->AsElement();
    if (HTMLEditUtils::IsListItem(childElement) &&
        !childContent->IsHTMLElement(&aNewListItemTag)) {
      // MOZ_KnownLive(childElement) because its lifetime is guaranteed by
      // listElementChildren.
      CreateElementResult newListItemElementOrError =
          ReplaceContainerWithTransaction(MOZ_KnownLive(*childElement),
                                          aNewListItemTag);
      if (newListItemElementOrError.isErr()) {
        NS_WARNING("HTMLEditor::ReplaceContainerWithTransaction() failed");
        return newListItemElementOrError;
      }
      newListItemElementOrError.MoveCaretPointTo(
          pointToPutCaret, {SuggestCaret::OnlyIfHasSuggestion});
      continue;
    }
    if (HTMLEditUtils::IsAnyListElement(childElement) &&
        !childElement->IsHTMLElement(&aNewListTag)) {
      // XXX List elements shouldn't have other list elements as their
      //     child.  Why do we handle such invalid tree?
      //     -> Maybe, for bug 525888.
      // MOZ_KnownLive(childElement) because its lifetime is guaranteed by
      // listElementChildren.
      CreateElementResult convertListTypeResult = ChangeListElementType(
          MOZ_KnownLive(*childElement), aNewListTag, aNewListItemTag);
      if (convertListTypeResult.isErr()) {
        NS_WARNING("HTMLEditor::ChangeListElementType() failed");
        return convertListTypeResult;
      }
      convertListTypeResult.MoveCaretPointTo(
          pointToPutCaret, {SuggestCaret::OnlyIfHasSuggestion});
      continue;
    }
  }

  if (aListElement.IsHTMLElement(&aNewListTag)) {
    return CreateElementResult(&aListElement, std::move(pointToPutCaret));
  }

  // XXX If we replace the list element, shouldn't we create it first and then,
  //     move children into it before inserting the new list element into the
  //     DOM tree? Then, we could reduce the cost of dispatching DOM mutation
  //     events.
  CreateElementResult listElementOrError =
      ReplaceContainerWithTransaction(aListElement, aNewListTag);
  if (listElementOrError.isErr()) {
    NS_WARNING("HTMLEditor::ReplaceContainerWithTransaction() failed");
    return listElementOrError;
  }
  listElementOrError.MoveCaretPointTo(pointToPutCaret,
                                      {SuggestCaret::OnlyIfHasSuggestion});
  return CreateElementResult(listElementOrError.UnwrapNewNode(),
                             std::move(pointToPutCaret));
}

Result<EditorDOMPoint, nsresult> HTMLEditor::CreateStyleForInsertText(
    const EditorDOMPoint& aPointToInsertText) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aPointToInsertText.IsSetAndValid());
  MOZ_ASSERT(mTypeInState);

  const RefPtr<Element> documentRootElement = GetDocument()->GetRootElement();
  if (NS_WARN_IF(!documentRootElement)) {
    return Err(NS_ERROR_FAILURE);
  }

  // process clearing any styles first
  UniquePtr<PropItem> item = mTypeInState->TakeClearProperty();

  EditorDOMPoint pointToPutCaret(aPointToInsertText);
  {
    // Transactions may set selection, but we will set selection if necessary.
    AutoTransactionsConserveSelection dontChangeMySelection(*this);

    while (item && pointToPutCaret.GetContainer() != documentRootElement) {
      Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
          ClearStyleAt(pointToPutCaret, MOZ_KnownLive(item->tag),
                       MOZ_KnownLive(item->attr), item->specifiedStyle);
      if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
        NS_WARNING("HTMLEditor::ClearStyleAt() failed");
        return pointToPutCaretOrError;
      }
      pointToPutCaret = pointToPutCaretOrError.unwrap();
      item = mTypeInState->TakeClearProperty();
    }
  }

  // then process setting any styles
  const int32_t relFontSize = mTypeInState->TakeRelativeFontSize();
  item = mTypeInState->TakeSetProperty();

  if (item || relFontSize) {
    // we have at least one style to add; make a new text node to insert style
    // nodes above.
    EditorDOMPoint pointToInsertTextNode(pointToPutCaret);
    if (pointToInsertTextNode.IsInTextNode()) {
      // if we are in a text node, split it
      SplitNodeResult splitTextNodeResult = SplitNodeDeepWithTransaction(
          MOZ_KnownLive(*pointToInsertTextNode.ContainerAs<Text>()),
          pointToInsertTextNode, SplitAtEdges::eAllowToCreateEmptyContainer);
      if (splitTextNodeResult.isErr()) {
        NS_WARNING(
            "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
            "eAllowToCreateEmptyContainer) failed");
        return Err(splitTextNodeResult.unwrapErr());
      }
      splitTextNodeResult.MoveCaretPointTo(
          pointToPutCaret, *this,
          {SuggestCaret::OnlyIfHasSuggestion,
           SuggestCaret::OnlyIfTransactionsAllowedToDoIt});
      pointToInsertTextNode =
          splitTextNodeResult.AtSplitPoint<EditorDOMPoint>();
    }
    if (!pointToInsertTextNode.IsInContentNode() ||
        !HTMLEditUtils::IsContainerNode(
            *pointToInsertTextNode.ContainerAs<nsIContent>())) {
      return pointToPutCaret;
    }
    RefPtr<Text> newEmptyTextNode = CreateTextNode(u""_ns);
    if (!newEmptyTextNode) {
      NS_WARNING("EditorBase::CreateTextNode() failed");
      return Err(NS_ERROR_FAILURE);
    }
    CreateTextResult insertNewTextNodeResult = InsertNodeWithTransaction<Text>(
        *newEmptyTextNode, pointToInsertTextNode);
    if (insertNewTextNodeResult.isErr()) {
      NS_WARNING("EditorBase::InsertNodeWithTransaction() failed");
      return Err(insertNewTextNodeResult.unwrapErr());
    }
    insertNewTextNodeResult.IgnoreCaretPointSuggestion();
    pointToPutCaret.Set(newEmptyTextNode, 0u);

    if (relFontSize) {
      HTMLEditor::FontSize incrementOrDecrement =
          relFontSize > 0 ? HTMLEditor::FontSize::incr
                          : HTMLEditor::FontSize::decr;
      for ([[maybe_unused]] uint32_t j : IntegerRange(Abs(relFontSize))) {
        const CreateElementResult wrapTextInBigOrSmallElementResult =
            SetFontSizeOnTextNode(*newEmptyTextNode, 0, UINT32_MAX,
                                  incrementOrDecrement);
        if (wrapTextInBigOrSmallElementResult.isErr()) {
          NS_WARNING("HTMLEditor::SetFontSizeOnTextNode() failed");
          return Err(wrapTextInBigOrSmallElementResult.inspectErr());
        }
        // We don't need to update here because we'll suggest caret position
        // which is computed above.
        MOZ_ASSERT(pointToPutCaret.IsSet());
        wrapTextInBigOrSmallElementResult.IgnoreCaretPointSuggestion();
      }
    }

    while (item) {
      Result<EditorDOMPoint, nsresult> setStyleResult = SetInlinePropertyOnNode(
          MOZ_KnownLive(*pointToPutCaret.ContainerAs<nsIContent>()),
          MOZ_KnownLive(*item->tag), MOZ_KnownLive(item->attr), item->value);
      if (MOZ_UNLIKELY(setStyleResult.isErr())) {
        NS_WARNING("HTMLEditor::SetInlinePropertyOnNode() failed");
        return Err(setStyleResult.unwrapErr());
      }
      // We don't need to update here because we'll suggest caret position which
      // is computed above.
      MOZ_ASSERT(pointToPutCaret.IsSet());
      item = mTypeInState->TakeSetProperty();
    }
  }

  return pointToPutCaret;
}

EditActionResult HTMLEditor::AlignAsSubAction(const nsAString& aAlignType,
                                              const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  AutoPlaceholderBatch treatAsOneTransaction(
      *this, ScrollSelectionIntoView::Yes, __FUNCTION__);
  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this, EditSubAction::eSetOrClearAlignment, nsIEditor::eNext,
      ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return EditActionResult(ignoredError.StealNSResult());
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Some selection containers are not content node, but ignored");
    return EditActionIgnored();
  }

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Mutation event listener might have changed the selection");
    return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  if (NS_SUCCEEDED(rv) && SelectionRef().IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterInvisibleBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterInvisibleBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  AutoRangeArray selectionRanges(SelectionRef());

  // XXX Why do we do this only when there is only one selection range?
  if (!selectionRanges.IsCollapsed() &&
      selectionRanges.Ranges().Length() == 1u) {
    Result<EditorRawDOMRange, nsresult> extendedRange =
        GetRangeExtendedToHardLineEdgesForBlockEditAction(
            selectionRanges.FirstRangeRef(), aEditingHost);
    if (MOZ_UNLIKELY(extendedRange.isErr())) {
      NS_WARNING(
          "HTMLEditor::GetRangeExtendedToHardLineEdgesForBlockEditAction() "
          "failed");
      return EditActionResult(extendedRange.unwrapErr());
    }
    // Note that end point may be prior to start point.  So, we
    // cannot use etStartAndEnd() here.
    nsresult rv = selectionRanges.SetBaseAndExtent(
        extendedRange.inspect().StartRef(), extendedRange.inspect().EndRef());
    if (NS_FAILED(rv)) {
      NS_WARNING("Selection::SetBaseAndExtentInLimiter() failed");
      return EditActionResult(rv);
    }
  }

  rv = AlignContentsAtRanges(selectionRanges, aAlignType, aEditingHost);
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::AlignContentsAtSelection() failed");
    return EditActionHandled(rv);
  }
  rv = selectionRanges.ApplyTo(SelectionRef());
  if (NS_FAILED(rv)) {
    NS_WARNING("AutoRangeArray::ApplyTo() failed");
    return EditActionHandled(rv);
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Mutation event listener might have changed the selection");
    return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  rv = MaybeInsertPaddingBRElementForEmptyLastLineAtSelection();
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::MaybeInsertPaddingBRElementForEmptyLastLineAtSelection() "
      "failed");
  return EditActionHandled(rv);
}

nsresult HTMLEditor::AlignContentsAtRanges(AutoRangeArray& aRanges,
                                           const nsAString& aAlignType,
                                           const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(aRanges.IsInContent());

  if (NS_WARN_IF(!aRanges.SaveAndTrackRanges(*this))) {
    return NS_ERROR_FAILURE;
  }

  EditorDOMPoint pointToPutCaret;

  // Convert the selection ranges into "promoted" selection ranges: This
  // basically just expands the range to include the immediate block parent,
  // and then further expands to include any ancestors whose children are all
  // in the range
  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  {
    AutoRangeArray extendedRanges(aRanges);
    extendedRanges.ExtendRangesToWrapLinesToHandleBlockLevelEditAction(
        EditSubAction::eSetOrClearAlignment, aEditingHost);
    Result<EditorDOMPoint, nsresult> splitResult =
        extendedRanges
            .SplitTextNodesAtEndBoundariesAndParentInlineElementsAtBoundaries(
                *this);
    if (MOZ_UNLIKELY(splitResult.isErr())) {
      NS_WARNING(
          "AutoRangeArray::"
          "SplitTextNodesAtEndBoundariesAndParentInlineElementsAtBoundaries() "
          "failed");
      return splitResult.unwrapErr();
    }
    if (splitResult.inspect().IsSet()) {
      pointToPutCaret = splitResult.unwrap();
    }
    nsresult rv = extendedRanges.CollectEditTargetNodes(
        *this, arrayOfContents, EditSubAction::eSetOrClearAlignment,
        AutoRangeArray::CollectNonEditableNodes::Yes);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "AutoRangeArray::CollectEditTargetNodes(EditSubAction::"
          "eSetOrClearAlignment, CollectNonEditableNodes::Yes) failed");
      return rv;
    }
  }

  Result<EditorDOMPoint, nsresult> splitAtBRElementsResult =
      MaybeSplitElementsAtEveryBRElement(arrayOfContents,
                                         EditSubAction::eSetOrClearAlignment);
  if (MOZ_UNLIKELY(splitAtBRElementsResult.isErr())) {
    NS_WARNING(
        "HTMLEditor::MaybeSplitElementsAtEveryBRElement(EditSubAction::"
        "eSetOrClearAlignment) failed");
    return splitAtBRElementsResult.inspectErr();
  }
  if (splitAtBRElementsResult.inspect().IsSet()) {
    pointToPutCaret = splitAtBRElementsResult.unwrap();
  }

  // If we don't have any nodes, or we have only a single br, then we are
  // creating an empty alignment div.  We have to do some different things for
  // these.
  bool createEmptyDivElement = arrayOfContents.IsEmpty();
  if (arrayOfContents.Length() == 1) {
    OwningNonNull<nsIContent>& content = arrayOfContents[0];

    if (HTMLEditUtils::SupportsAlignAttr(content)) {
      // The node is a table element, an hr, a paragraph, a div or a section
      // header; in HTML 4, it can directly carry the ALIGN attribute and we
      // don't need to make a div! If we are in CSS mode, all the work is done
      // in SetBlockElementAlign().
      Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
          SetBlockElementAlign(MOZ_KnownLive(*content->AsElement()), aAlignType,
                               EditTarget::OnlyDescendantsExceptTable);
      if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
        NS_WARNING("HTMLEditor::SetBlockElementAlign() failed");
        return pointToPutCaretOrError.unwrapErr();
      }
      if (pointToPutCaretOrError.inspect().IsSet()) {
        pointToPutCaret = pointToPutCaretOrError.unwrap();
      }
    }

    if (content->IsHTMLElement(nsGkAtoms::br)) {
      // The special case createEmptyDivElement code (below) that consumes
      // `<br>` elements can cause tables to split if the start node of the
      // selection is not in a table cell or caption, for example parent is a
      // `<tr>`.  Avoid this unnecessary splitting if possible by leaving
      // createEmptyDivElement false so that we fall through to the normal case
      // alignment code.
      //
      // XXX: It seems a little error prone for the createEmptyDivElement
      //      special case code to assume that the start node of the selection
      //      is the parent of the single node in the arrayOfContents, as the
      //      paragraph above points out. Do we rely on the selection start
      //      node because of the fact that arrayOfContents can be empty?  We
      //      should probably revisit this issue. - kin

      const EditorDOMPoint firstRangeStartPoint =
          pointToPutCaret.IsSet()
              ? pointToPutCaret
              : aRanges.GetFirstRangeStartPoint<EditorDOMPoint>();
      if (NS_WARN_IF(!firstRangeStartPoint.IsSet())) {
        return NS_ERROR_FAILURE;
      }
      nsINode* parent = firstRangeStartPoint.GetContainer();
      createEmptyDivElement = !HTMLEditUtils::IsAnyTableElement(parent) ||
                              HTMLEditUtils::IsTableCellOrCaption(*parent);
    }
  }

  if (createEmptyDivElement) {
    if (MOZ_UNLIKELY(!pointToPutCaret.IsSet() && !aRanges.IsInContent())) {
      NS_WARNING("Mutaiton event listener might have changed the selection");
      return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
    }
    const EditorDOMPoint pointToInsertDivElement =
        pointToPutCaret.IsSet() ? pointToPutCaret
                                : GetFirstSelectionStartPoint<EditorDOMPoint>();
    CreateElementResult newDivElementOrError = InsertDivElementToAlignContents(
        pointToInsertDivElement, aAlignType, aEditingHost);
    if (newDivElementOrError.isErr()) {
      NS_WARNING("HTMLEditor::InsertDivElementToAlignContents() failed");
      return newDivElementOrError.unwrapErr();
    }
    aRanges.ClearSavedRanges();
    EditorDOMPoint pointToPutCaret = newDivElementOrError.UnwrapCaretPoint();
    nsresult rv = aRanges.Collapse(pointToPutCaret);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "AutoRangeArray::Collapse() failed");
    return rv;
  }

  CreateElementResult maybeCreateDivElementResult =
      AlignNodesAndDescendants(arrayOfContents, aAlignType, aEditingHost);
  if (maybeCreateDivElementResult.isErr()) {
    NS_WARNING("HTMLEditor::AlignNodesAndDescendants() failed");
    return maybeCreateDivElementResult.unwrapErr();
  }
  maybeCreateDivElementResult.IgnoreCaretPointSuggestion();

  MOZ_ASSERT(aRanges.HasSavedRanges());
  aRanges.RestoreFromSavedRanges();
  // If restored range is collapsed outside the latest cased <div> element,
  // we should move caret into the <div>.
  if (maybeCreateDivElementResult.GetNewNode() && aRanges.IsCollapsed() &&
      !aRanges.Ranges().IsEmpty()) {
    const auto firstRangeStartRawPoint =
        aRanges.GetFirstRangeStartPoint<EditorRawDOMPoint>();
    if (MOZ_LIKELY(firstRangeStartRawPoint.IsSet())) {
      Result<EditorRawDOMPoint, nsresult> pointInNewDivOrError =
          HTMLEditUtils::ComputePointToPutCaretInElementIfOutside<
              EditorRawDOMPoint>(*maybeCreateDivElementResult.GetNewNode(),
                                 firstRangeStartRawPoint);
      if (MOZ_UNLIKELY(pointInNewDivOrError.isErr())) {
        NS_WARNING(
            "HTMLEditUtils::ComputePointToPutCaretInElementIfOutside() failed, "
            "but ignored");
      } else if (pointInNewDivOrError.inspect().IsSet()) {
        nsresult rv = aRanges.Collapse(pointInNewDivOrError.unwrap());
        if (NS_FAILED(rv)) {
          NS_WARNING("AutoRangeArray::Collapse() failed");
          return rv;
        }
      }
    }
  }
  return NS_OK;
}

CreateElementResult HTMLEditor::InsertDivElementToAlignContents(
    const EditorDOMPoint& aPointToInsert, const nsAString& aAlignType,
    const Element& aEditingHost) {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());
  MOZ_ASSERT(!IsSelectionRangeContainerNotContent());
  MOZ_ASSERT(aPointToInsert.IsSetAndValid());

  if (NS_WARN_IF(!aPointToInsert.IsSet())) {
    return CreateElementResult(NS_ERROR_FAILURE);
  }

  CreateElementResult createNewDivElementResult =
      InsertElementWithSplittingAncestorsWithTransaction(
          *nsGkAtoms::div, aPointToInsert, BRElementNextToSplitPoint::Delete,
          aEditingHost);
  if (createNewDivElementResult.isErr()) {
    NS_WARNING(
        "HTMLEditor::InsertElementWithSplittingAncestorsWithTransaction("
        "nsGkAtoms::div, BRElementNextToSplitPoint::Delete) failed");
    return createNewDivElementResult;
  }
  // We'll suggest start of the new <div>, so we don't need the suggested
  // position.
  createNewDivElementResult.IgnoreCaretPointSuggestion();

  MOZ_ASSERT(createNewDivElementResult.GetNewNode());
  RefPtr<Element> newDivElement = createNewDivElementResult.UnwrapNewNode();
  // Set up the alignment on the div, using HTML or CSS
  Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
      SetBlockElementAlign(*newDivElement, aAlignType,
                           EditTarget::OnlyDescendantsExceptTable);
  if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
    NS_WARNING(
        "HTMLEditor::SetBlockElementAlign(EditTarget::"
        "OnlyDescendantsExceptTable) failed");
    return CreateElementResult(pointToPutCaretOrError.unwrapErr());
  }
  // We don't need the new suggested position too.

  // Put in a padding <br> element for empty last line so that it won't get
  // deleted.
  CreateElementResult insertPaddingBRElementResult =
      InsertPaddingBRElementForEmptyLastLineWithTransaction(
          EditorDOMPoint(newDivElement, 0u));
  if (insertPaddingBRElementResult.isErr()) {
    NS_WARNING(
        "HTMLEditor::InsertPaddingBRElementForEmptyLastLineWithTransaction() "
        "failed");
    return insertPaddingBRElementResult;
  }
  insertPaddingBRElementResult.IgnoreCaretPointSuggestion();

  return CreateElementResult(newDivElement, EditorDOMPoint(newDivElement, 0u));
}

CreateElementResult HTMLEditor::AlignNodesAndDescendants(
    nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents,
    const nsAString& aAlignType, const Element& aEditingHost) {
  // Detect all the transitions in the array, where a transition means that
  // adjacent nodes in the array don't have the same parent.
  AutoTArray<bool, 64> transitionList;
  HTMLEditor::MakeTransitionList(aArrayOfContents, transitionList);

  RefPtr<Element> latestCreatedDivElement;
  EditorDOMPoint pointToPutCaret;

  // Okay, now go through all the nodes and give them an align attrib or put
  // them in a div, or whatever is appropriate.  Woohoo!

  RefPtr<Element> createdDivElement;
  const bool useCSS = IsCSSEnabled();
  int32_t indexOfTransitionList = -1;
  for (OwningNonNull<nsIContent>& content : aArrayOfContents) {
    ++indexOfTransitionList;

    // Ignore all non-editable nodes.  Leave them be.
    if (!EditorUtils::IsEditableContent(content, EditorType::HTML)) {
      continue;
    }

    // The node is a table element, an hr, a paragraph, a div or a section
    // header; in HTML 4, it can directly carry the ALIGN attribute and we
    // don't need to nest it, just set the alignment.  In CSS, assign the
    // corresponding CSS styles in SetBlockElementAlign().
    if (HTMLEditUtils::SupportsAlignAttr(content)) {
      Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
          SetBlockElementAlign(MOZ_KnownLive(*content->AsElement()), aAlignType,
                               EditTarget::NodeAndDescendantsExceptTable);
      if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
        NS_WARNING(
            "HTMLEditor::SetBlockElementAlign(EditTarget::"
            "NodeAndDescendantsExceptTable) failed");
        return CreateElementResult(pointToPutCaretOrError.unwrapErr());
      }
      if (pointToPutCaretOrError.inspect().IsSet()) {
        pointToPutCaret = pointToPutCaretOrError.unwrap();
      }
      // Clear out createdDivElement so that we don't put nodes after this one
      // into it
      createdDivElement = nullptr;
      continue;
    }

    EditorDOMPoint atContent(content);
    if (NS_WARN_IF(!atContent.IsSet())) {
      continue;
    }

    // Skip insignificant formatting text nodes to prevent unnecessary
    // structure splitting!
    if (content->IsText() &&
        ((HTMLEditUtils::IsAnyTableElement(atContent.GetContainer()) &&
          !HTMLEditUtils::IsTableCellOrCaption(*atContent.GetContainer())) ||
         HTMLEditUtils::IsAnyListElement(atContent.GetContainer()) ||
         HTMLEditUtils::IsEmptyNode(
             *content, {EmptyCheckOption::TreatSingleBRElementAsVisible}))) {
      continue;
    }

    // If it's a list item, or a list inside a list, forget any "current" div,
    // and instead put divs inside the appropriate block (td, li, etc.)
    if (HTMLEditUtils::IsListItem(content) ||
        HTMLEditUtils::IsAnyListElement(content)) {
      Element* listOrListItemElement = content->AsElement();
      AutoEditorDOMPointOffsetInvalidator lockChild(atContent);
      // MOZ_KnownLive(*listOrListItemElement): An element of aArrayOfContents
      // which is array of OwningNonNull.
      Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
          RemoveAlignFromDescendants(MOZ_KnownLive(*listOrListItemElement),
                                     aAlignType,
                                     EditTarget::OnlyDescendantsExceptTable);
      if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
        NS_WARNING(
            "HTMLEditor::RemoveAlignFromDescendants(EditTarget::"
            "OnlyDescendantsExceptTable) failed");
        return CreateElementResult(pointToPutCaretOrError.unwrapErr());
      }
      if (pointToPutCaretOrError.inspect().IsSet()) {
        pointToPutCaret = pointToPutCaretOrError.unwrap();
      }

      if (useCSS) {
        if (nsStyledElement* styledListOrListItemElement =
                nsStyledElement::FromNode(listOrListItemElement)) {
          // MOZ_KnownLive(*styledListOrListItemElement): An element of
          // aArrayOfContents which is array of OwningNonNull.
          Result<int32_t, nsresult> result =
              mCSSEditUtils->SetCSSEquivalentToHTMLStyleWithTransaction(
                  MOZ_KnownLive(*styledListOrListItemElement), nullptr,
                  nsGkAtoms::align, &aAlignType);
          if (result.isErr()) {
            if (result.inspectErr() == NS_ERROR_EDITOR_DESTROYED) {
              NS_WARNING(
                  "CSSEditUtils::SetCSSEquivalentToHTMLStyleWithTransaction("
                  "nsGkAtoms::align) destroyed the editor");
              return CreateElementResult(result.unwrapErr());
            }
            NS_WARNING(
                "CSSEditUtils::SetCSSEquivalentToHTMLStyleWithTransaction("
                "nsGkAtoms::align) failed, but ignored");
          }
        }
        createdDivElement = nullptr;
        continue;
      }

      if (HTMLEditUtils::IsAnyListElement(atContent.GetContainer())) {
        // If we don't use CSS, add a content to list element: they have to
        // be inside another list, i.e., >= second level of nesting.
        // XXX AlignContentsInAllTableCellsAndListItems() handles only list
        //     item elements and table cells.  Is it intentional?  Why don't
        //     we need to align contents in other type blocks?
        // MOZ_KnownLive(*listOrListItemElement): An element of aArrayOfContents
        // which is array of OwningNonNull.
        Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
            AlignContentsInAllTableCellsAndListItems(
                MOZ_KnownLive(*listOrListItemElement), aAlignType);
        if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
          NS_WARNING(
              "HTMLEditor::AlignContentsInAllTableCellsAndListItems() failed");
          return CreateElementResult(pointToPutCaretOrError.unwrapErr());
        }
        if (pointToPutCaretOrError.inspect().IsSet()) {
          pointToPutCaret = pointToPutCaretOrError.unwrap();
        }
        createdDivElement = nullptr;
        continue;
      }

      // Clear out createdDivElement so that we don't put nodes after this one
      // into it
    }

    // Need to make a div to put things in if we haven't already, or if this
    // node doesn't go in div we used earlier.
    if (!createdDivElement || transitionList[indexOfTransitionList]) {
      // First, check that our element can contain a div.
      if (!HTMLEditUtils::CanNodeContain(*atContent.GetContainer(),
                                         *nsGkAtoms::div)) {
        // XXX Why do we return "OK" here rather than returning error or
        //     doing continue?
        return latestCreatedDivElement
                   ? CreateElementResult(std::move(latestCreatedDivElement),
                                         std::move(pointToPutCaret))
                   : CreateElementResult::NotHandled(
                         std::move(pointToPutCaret));
      }

      CreateElementResult createNewDivElementResult =
          InsertElementWithSplittingAncestorsWithTransaction(
              *nsGkAtoms::div, atContent, BRElementNextToSplitPoint::Keep,
              aEditingHost);
      if (createNewDivElementResult.isErr()) {
        NS_WARNING(
            "HTMLEditor::InsertElementWithSplittingAncestorsWithTransaction("
            "nsGkAtoms::div) failed");
        return createNewDivElementResult;
      }
      if (createNewDivElementResult.HasCaretPointSuggestion()) {
        pointToPutCaret = createNewDivElementResult.UnwrapCaretPoint();
      }

      MOZ_ASSERT(createNewDivElementResult.GetNewNode());
      createdDivElement = createNewDivElementResult.UnwrapNewNode();
      // Set up the alignment on the div
      Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
          SetBlockElementAlign(*createdDivElement, aAlignType,
                               EditTarget::OnlyDescendantsExceptTable);
      if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
        if (MOZ_UNLIKELY(pointToPutCaretOrError.inspectErr() ==
                         NS_ERROR_EDITOR_DESTROYED)) {
          NS_WARNING(
              "HTMLEditor::SetBlockElementAlign(EditTarget::"
              "OnlyDescendantsExceptTable) failed");
          return CreateElementResult(pointToPutCaretOrError.unwrapErr());
        }
        NS_WARNING(
            "HTMLEditor::SetBlockElementAlign(EditTarget::"
            "OnlyDescendantsExceptTable) failed, but ignored");
      } else if (pointToPutCaretOrError.inspect().IsSet()) {
        pointToPutCaret = pointToPutCaretOrError.unwrap();
      }
      latestCreatedDivElement = createdDivElement;
    }

    // Tuck the node into the end of the active div
    //
    // MOZ_KnownLive because 'aArrayOfContents' is guaranteed to keep it alive.
    MoveNodeResult moveNodeResult = MoveNodeToEndWithTransaction(
        MOZ_KnownLive(content), *createdDivElement);
    if (moveNodeResult.isErr()) {
      NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
      return CreateElementResult(moveNodeResult.unwrapErr());
    }
    if (moveNodeResult.HasCaretPointSuggestion()) {
      pointToPutCaret = moveNodeResult.UnwrapCaretPoint();
    }
  }

  return latestCreatedDivElement
             ? CreateElementResult(std::move(latestCreatedDivElement),
                                   std::move(pointToPutCaret))
             : CreateElementResult::NotHandled(std::move(pointToPutCaret));
}

Result<EditorDOMPoint, nsresult>
HTMLEditor::AlignContentsInAllTableCellsAndListItems(
    Element& aElement, const nsAString& aAlignType) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  // Gather list of table cells or list items
  AutoTArray<OwningNonNull<Element>, 64> arrayOfTableCellsAndListItems;
  DOMIterator iter(aElement);
  iter.AppendNodesToArray(
      +[](nsINode& aNode, void*) -> bool {
        MOZ_ASSERT(Element::FromNode(&aNode));
        return HTMLEditUtils::IsTableCell(&aNode) ||
               HTMLEditUtils::IsListItem(&aNode);
      },
      arrayOfTableCellsAndListItems);

  // Now that we have the list, align their contents as requested
  EditorDOMPoint pointToPutCaret;
  for (auto& tableCellOrListItemElement : arrayOfTableCellsAndListItems) {
    // MOZ_KnownLive because 'arrayOfTableCellsAndListItems' is guaranteed to
    // keep it alive.
    Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
        AlignBlockContentsWithDivElement(
            MOZ_KnownLive(tableCellOrListItemElement), aAlignType);
    if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
      NS_WARNING("HTMLEditor::AlignBlockContentsWithDivElement() failed");
      return pointToPutCaretOrError;
    }
    if (pointToPutCaretOrError.inspect().IsSet()) {
      pointToPutCaret = pointToPutCaretOrError.unwrap();
    }
  }

  return pointToPutCaret;
}

Result<EditorDOMPoint, nsresult> HTMLEditor::AlignBlockContentsWithDivElement(
    Element& aBlockElement, const nsAString& aAlignType) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  // XXX I don't understand why we should NOT align non-editable children
  //     with modifying EDITABLE `<div>` element.
  nsCOMPtr<nsIContent> firstEditableContent = HTMLEditUtils::GetFirstChild(
      aBlockElement, {WalkTreeOption::IgnoreNonEditableNode});
  if (!firstEditableContent) {
    // This block has no editable content, nothing to align.
    return EditorDOMPoint();
  }

  // If there is only one editable content and it's a `<div>` element,
  // just set `align` attribute of it.
  nsCOMPtr<nsIContent> lastEditableContent = HTMLEditUtils::GetLastChild(
      aBlockElement, {WalkTreeOption::IgnoreNonEditableNode});
  if (firstEditableContent == lastEditableContent &&
      firstEditableContent->IsHTMLElement(nsGkAtoms::div)) {
    nsresult rv = SetAttributeOrEquivalent(
        MOZ_KnownLive(firstEditableContent->AsElement()), nsGkAtoms::align,
        aAlignType, false);
    if (NS_WARN_IF(Destroyed())) {
      NS_WARNING(
          "EditorBase::SetAttributeOrEquivalent(nsGkAtoms::align) caused "
          "destroying the editor");
      return Err(NS_ERROR_EDITOR_DESTROYED);
    }
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "EditorBase::SetAttributeOrEquivalent(nsGkAtoms::align) failed");
      return Err(rv);
    }
    return EditorDOMPoint();
  }

  // Otherwise, we need to insert a `<div>` element to set `align` attribute.
  // XXX Don't insert the new `<div>` element until we set `align` attribute
  //     for avoiding running mutation event listeners.
  CreateElementResult createNewDivElementResult = CreateAndInsertElement(
      WithTransaction::Yes, *nsGkAtoms::div, EditorDOMPoint(&aBlockElement, 0u),
      // MOZ_CAN_RUN_SCRIPT_BOUNDARY due to bug 1758868
      [&aAlignType](HTMLEditor& aHTMLEditor, Element& aDivElement,
                    const EditorDOMPoint&) MOZ_CAN_RUN_SCRIPT_BOUNDARY {
        // If aDivElement has not been connected yet, we do not need
        // transaction of setting align attribute here.
        nsresult rv = aHTMLEditor.SetAttributeOrEquivalent(
            &aDivElement, nsGkAtoms::align, aAlignType,
            !aDivElement.IsInComposedDoc());
        NS_WARNING_ASSERTION(
            NS_SUCCEEDED(rv),
            nsPrintfCString("EditorBase::SetAttributeOrEquivalent(nsGkAtoms:: "
                            "align, \"...\", %s) failed",
                            !aDivElement.IsInComposedDoc() ? "true" : "false")
                .get());
        return rv;
      });
  if (createNewDivElementResult.isErr()) {
    NS_WARNING(
        "HTMLEditor::CreateAndInsertElement(WithTransaction::Yes, "
        "nsGkAtoms::div) failed");
    return Err(createNewDivElementResult.unwrapErr());
  }
  EditorDOMPoint pointToPutCaret = createNewDivElementResult.UnwrapCaretPoint();

  RefPtr<Element> newDivElement = createNewDivElementResult.UnwrapNewNode();
  MOZ_ASSERT(newDivElement);
  // XXX This is tricky and does not work with mutation event listeners.
  //     But I'm not sure what we should do if new content is inserted.
  //     Anyway, I don't think that we should move editable contents
  //     over non-editable contents.  Chrome does no do that.
  while (lastEditableContent && (lastEditableContent != newDivElement)) {
    MoveNodeResult moveNodeResult = MoveNodeWithTransaction(
        *lastEditableContent, EditorDOMPoint(newDivElement, 0u));
    if (moveNodeResult.isErr()) {
      NS_WARNING("HTMLEditor::MoveNodeWithTransaction() failed");
      return Err(moveNodeResult.unwrapErr());
    }
    if (moveNodeResult.HasCaretPointSuggestion()) {
      pointToPutCaret = moveNodeResult.UnwrapCaretPoint();
    }
    lastEditableContent = HTMLEditUtils::GetLastChild(
        aBlockElement, {WalkTreeOption::IgnoreNonEditableNode});
  }
  return pointToPutCaret;
}

Result<EditorRawDOMRange, nsresult>
HTMLEditor::GetRangeExtendedToHardLineEdgesForBlockEditAction(
    const nsRange* aRange, const Element& aEditingHost) const {
  MOZ_ASSERT(IsEditActionDataAvailable());

  // This tweaks selections to be more "natural".
  // Idea here is to adjust edges of selection ranges so that they do not cross
  // breaks or block boundaries unless something editable beyond that boundary
  // is also selected.  This adjustment makes it much easier for the various
  // block operations to determine what nodes to act on.
  if (NS_WARN_IF(!aRange) || NS_WARN_IF(!aRange->IsPositioned())) {
    return Err(NS_ERROR_FAILURE);
  }

  const EditorRawDOMPoint startPoint(aRange->StartRef());
  if (NS_WARN_IF(!startPoint.IsSet())) {
    return Err(NS_ERROR_FAILURE);
  }
  const EditorRawDOMPoint endPoint(aRange->EndRef());
  if (NS_WARN_IF(!endPoint.IsSet())) {
    return Err(NS_ERROR_FAILURE);
  }

  // adjusted values default to original values
  EditorRawDOMRange newRange(startPoint, endPoint);

  // Is there any intervening visible white-space?  If so we can't push
  // selection past that, it would visibly change meaning of users selection.
  WSRunScanner wsScannerAtEnd(&aEditingHost, endPoint);
  WSScanResult scanResultAtEnd =
      wsScannerAtEnd.ScanPreviousVisibleNodeOrBlockBoundaryFrom(endPoint);
  if (scanResultAtEnd.Failed()) {
    NS_WARNING(
        "WSRunScanner::ScanPreviousVisibleNodeOrBlockBoundaryFrom() failed");
    return Err(NS_ERROR_FAILURE);
  }
  if (scanResultAtEnd.ReachedSomethingNonTextContent()) {
    // eThisBlock and eOtherBlock conveniently distinguish cases
    // of going "down" into a block and "up" out of a block.
    if (wsScannerAtEnd.StartsFromOtherBlockElement()) {
      // endpoint is just after the close of a block.
      if (nsIContent* child = HTMLEditUtils::GetLastLeafContent(
              *wsScannerAtEnd.StartReasonOtherBlockElementPtr(),
              {LeafNodeType::LeafNodeOrChildBlock})) {
        newRange.SetEnd(EditorRawDOMPoint::After(*child));
      }
      // else block is empty - we can leave selection alone here, i think.
    } else if (wsScannerAtEnd.StartsFromCurrentBlockBoundary()) {
      // endpoint is just after start of this block
      if (nsIContent* child = HTMLEditUtils::GetPreviousContent(
              endPoint, {WalkTreeOption::IgnoreNonEditableNode},
              &aEditingHost)) {
        newRange.SetEnd(EditorRawDOMPoint::After(*child));
      }
      // else block is empty - we can leave selection alone here, i think.
    } else if (wsScannerAtEnd.StartsFromBRElement()) {
      // endpoint is just after break.  lets adjust it to before it.
      newRange.SetEnd(
          EditorRawDOMPoint(wsScannerAtEnd.StartReasonBRElementPtr()));
    }
  }

  // Is there any intervening visible white-space?  If so we can't push
  // selection past that, it would visibly change meaning of users selection.
  WSRunScanner wsScannerAtStart(&aEditingHost, startPoint);
  WSScanResult scanResultAtStart =
      wsScannerAtStart.ScanNextVisibleNodeOrBlockBoundaryFrom(startPoint);
  if (scanResultAtStart.Failed()) {
    NS_WARNING("WSRunScanner::ScanNextVisibleNodeOrBlockBoundaryFrom() failed");
    return Err(NS_ERROR_FAILURE);
  }
  if (scanResultAtStart.ReachedSomethingNonTextContent()) {
    // eThisBlock and eOtherBlock conveniently distinguish cases
    // of going "down" into a block and "up" out of a block.
    if (wsScannerAtStart.EndsByOtherBlockElement()) {
      // startpoint is just before the start of a block.
      if (nsIContent* child = HTMLEditUtils::GetFirstLeafContent(
              *wsScannerAtStart.EndReasonOtherBlockElementPtr(),
              {LeafNodeType::LeafNodeOrChildBlock})) {
        newRange.SetStart(EditorRawDOMPoint(child));
      }
      // else block is empty - we can leave selection alone here, i think.
    } else if (wsScannerAtStart.EndsByCurrentBlockBoundary()) {
      // startpoint is just before end of this block
      if (nsIContent* child = HTMLEditUtils::GetNextContent(
              startPoint, {WalkTreeOption::IgnoreNonEditableNode},
              &aEditingHost)) {
        newRange.SetStart(EditorRawDOMPoint(child));
      }
      // else block is empty - we can leave selection alone here, i think.
    } else if (wsScannerAtStart.EndsByBRElement()) {
      // startpoint is just before a break.  lets adjust it to after it.
      newRange.SetStart(
          EditorRawDOMPoint::After(*wsScannerAtStart.EndReasonBRElementPtr()));
    }
  }

  // There is a demented possibility we have to check for.  We might have a very
  // strange selection that is not collapsed and yet does not contain any
  // editable content, and satisfies some of the above conditions that cause
  // tweaking.  In this case we don't want to tweak the selection into a block
  // it was never in, etc.  There are a variety of strategies one might use to
  // try to detect these cases, but I think the most straightforward is to see
  // if the adjusted locations "cross" the old values: i.e., new end before old
  // start, or new start after old end.  If so then just leave things alone.

  Maybe<int32_t> comp = nsContentUtils::ComparePoints(
      startPoint.ToRawRangeBoundary(), newRange.EndRef().ToRawRangeBoundary());

  if (NS_WARN_IF(!comp)) {
    return Err(NS_ERROR_FAILURE);
  }

  if (*comp == 1) {
    return EditorRawDOMRange();  // New end before old start.
  }

  comp = nsContentUtils::ComparePoints(newRange.StartRef().ToRawRangeBoundary(),
                                       endPoint.ToRawRangeBoundary());

  if (NS_WARN_IF(!comp)) {
    return Err(NS_ERROR_FAILURE);
  }

  if (*comp == 1) {
    return EditorRawDOMRange();  // New start after old end.
  }

  return newRange;
}

template <typename EditorDOMRangeType>
already_AddRefed<nsRange> HTMLEditor::CreateRangeIncludingAdjuscentWhiteSpaces(
    const EditorDOMRangeType& aRange) {
  MOZ_DIAGNOSTIC_ASSERT(aRange.IsPositioned());
  return CreateRangeIncludingAdjuscentWhiteSpaces(aRange.StartRef(),
                                                  aRange.EndRef());
}

template <typename EditorDOMPointType1, typename EditorDOMPointType2>
already_AddRefed<nsRange> HTMLEditor::CreateRangeIncludingAdjuscentWhiteSpaces(
    const EditorDOMPointType1& aStartPoint,
    const EditorDOMPointType2& aEndPoint) {
  MOZ_DIAGNOSTIC_ASSERT(!aStartPoint.IsInNativeAnonymousSubtree());
  MOZ_DIAGNOSTIC_ASSERT(!aEndPoint.IsInNativeAnonymousSubtree());

  if (!aStartPoint.IsInContentNode() || !aEndPoint.IsInContentNode()) {
    NS_WARNING_ASSERTION(aStartPoint.IsSet(), "aStartPoint was not set");
    NS_WARNING_ASSERTION(aEndPoint.IsSet(), "aEndPoint was not set");
    return nullptr;
  }

  const Element* const editingHost = ComputeEditingHost();
  if (NS_WARN_IF(!editingHost)) {
    return nullptr;
  }

  EditorDOMPoint startPoint = aStartPoint.template To<EditorDOMPoint>();
  EditorDOMPoint endPoint = aEndPoint.template To<EditorDOMPoint>();
  AutoRangeArray::UpdatePointsToSelectAllChildrenIfCollapsedInEmptyBlockElement(
      startPoint, endPoint, *editingHost);

  if (NS_WARN_IF(!startPoint.IsInContentNode()) ||
      NS_WARN_IF(!endPoint.IsInContentNode())) {
    NS_WARNING(
        "AutoRangeArray::"
        "UpdatePointsToSelectAllChildrenIfCollapsedInEmptyBlockElement() "
        "failed");
    return nullptr;
  }

  // For text actions, we want to look backwards (or forwards, as
  // appropriate) for additional white-space or nbsp's.  We may have to act
  // on these later even though they are outside of the initial selection.
  // Even if they are in another node!
  // XXX Those scanners do not treat siblings of the text nodes.  Perhaps,
  //     we should use `WSRunScanner::GetFirstASCIIWhiteSpacePointCollapsedTo()`
  //     and `WSRunScanner::GetEndOfCollapsibleASCIIWhiteSpaces()` instead.
  if (startPoint.IsInTextNode()) {
    while (!startPoint.IsStartOfContainer()) {
      if (!startPoint.IsPreviousCharASCIISpaceOrNBSP()) {
        break;
      }
      MOZ_ALWAYS_TRUE(startPoint.RewindOffset());
    }
  }
  if (!startPoint.GetChildOrContainerIfDataNode() ||
      !startPoint.GetChildOrContainerIfDataNode()->IsInclusiveDescendantOf(
          editingHost)) {
    return nullptr;
  }
  if (endPoint.IsInTextNode()) {
    while (!endPoint.IsEndOfContainer()) {
      if (!endPoint.IsCharASCIISpaceOrNBSP()) {
        break;
      }
      MOZ_ALWAYS_TRUE(endPoint.AdvanceOffset());
    }
  }
  EditorDOMPoint lastRawPoint(endPoint);
  if (!lastRawPoint.IsStartOfContainer()) {
    lastRawPoint.RewindOffset();
  }
  if (!lastRawPoint.GetChildOrContainerIfDataNode() ||
      !lastRawPoint.GetChildOrContainerIfDataNode()->IsInclusiveDescendantOf(
          editingHost)) {
    return nullptr;
  }

  RefPtr<nsRange> range =
      nsRange::Create(startPoint.ToRawRangeBoundary(),
                      endPoint.ToRawRangeBoundary(), IgnoreErrors());
  NS_WARNING_ASSERTION(range, "nsRange::Create() failed");
  return range.forget();
}

Result<EditorDOMPoint, nsresult> HTMLEditor::MaybeSplitElementsAtEveryBRElement(
    nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents,
    EditSubAction aEditSubAction) {
  // Post-process the list to break up inline containers that contain br's, but
  // only for operations that might care, like making lists or paragraphs
  switch (aEditSubAction) {
    case EditSubAction::eCreateOrRemoveBlock:
    case EditSubAction::eMergeBlockContents:
    case EditSubAction::eCreateOrChangeList:
    case EditSubAction::eSetOrClearAlignment:
    case EditSubAction::eSetPositionToAbsolute:
    case EditSubAction::eIndent:
    case EditSubAction::eOutdent: {
      EditorDOMPoint pointToPutCaret;
      for (int32_t i = aArrayOfContents.Length() - 1; i >= 0; i--) {
        OwningNonNull<nsIContent>& content = aArrayOfContents[i];
        if (HTMLEditUtils::IsInlineElement(content) &&
            HTMLEditUtils::IsContainerNode(content) && !content->IsText()) {
          AutoTArray<OwningNonNull<nsIContent>, 24> arrayOfInlineContents;
          // MOZ_KnownLive because 'aArrayOfContents' is guaranteed to keep it
          // alive.
          Result<EditorDOMPoint, nsresult> splitResult =
              SplitElementsAtEveryBRElement(MOZ_KnownLive(content),
                                            arrayOfInlineContents);
          if (splitResult.isErr()) {
            NS_WARNING("HTMLEditor::SplitElementsAtEveryBRElement() failed");
            return splitResult;
          }
          if (splitResult.inspect().IsSet()) {
            pointToPutCaret = splitResult.unwrap();
          }
          // Put these nodes in aArrayOfContents, replacing the current node
          aArrayOfContents.RemoveElementAt(i);
          aArrayOfContents.InsertElementsAt(i, arrayOfInlineContents);
        }
      }
      return pointToPutCaret;
    }
    default:
      return EditorDOMPoint();
  }
}

Result<EditorDOMPoint, nsresult>
HTMLEditor::SplitParentInlineElementsAtRangeEdges(RangeItem& aRangeItem) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  RefPtr<Element> editingHost = ComputeEditingHost();
  if (NS_WARN_IF(!editingHost)) {
    return EditorDOMPoint();  // XXX Why not an error?
  }

  EditorDOMPoint pointToPutCaret;
  if (!aRangeItem.Collapsed() && aRangeItem.mEndContainer &&
      aRangeItem.mEndContainer->IsContent()) {
    nsCOMPtr<nsIContent> mostAncestorInlineContentAtEnd =
        HTMLEditUtils::GetMostDistantAncestorInlineElement(
            *aRangeItem.mEndContainer->AsContent(), editingHost);

    if (mostAncestorInlineContentAtEnd) {
      SplitNodeResult splitEndInlineResult = SplitNodeDeepWithTransaction(
          *mostAncestorInlineContentAtEnd, aRangeItem.EndPoint(),
          SplitAtEdges::eDoNotCreateEmptyContainer);
      if (splitEndInlineResult.isErr()) {
        NS_WARNING(
            "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
            "eDoNotCreateEmptyContainer) failed");
        return Err(splitEndInlineResult.unwrapErr());
      }
      splitEndInlineResult.MoveCaretPointTo(
          pointToPutCaret, {SuggestCaret::OnlyIfHasSuggestion});
      if (pointToPutCaret.IsInContentNode() &&
          MOZ_UNLIKELY(
              editingHost !=
              ComputeEditingHost(*pointToPutCaret.ContainerAs<nsIContent>()))) {
        NS_WARNING(
            "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
            "eDoNotCreateEmptyContainer) caused changing editing host");
        return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
      }
      const EditorRawDOMPoint& splitPointAtEnd =
          splitEndInlineResult.AtSplitPoint<EditorRawDOMPoint>();
      if (MOZ_UNLIKELY(!splitPointAtEnd.IsSet())) {
        NS_WARNING(
            "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
            "eDoNotCreateEmptyContainer) didn't return split point");
        return Err(NS_ERROR_FAILURE);
      }
      aRangeItem.mEndContainer = splitPointAtEnd.GetContainer();
      aRangeItem.mEndOffset = splitPointAtEnd.Offset();
    }
  }

  if (!aRangeItem.mStartContainer || !aRangeItem.mStartContainer->IsContent()) {
    return pointToPutCaret;
  }

  nsCOMPtr<nsIContent> mostAncestorInlineContentAtStart =
      HTMLEditUtils::GetMostDistantAncestorInlineElement(
          *aRangeItem.mStartContainer->AsContent(), editingHost);

  if (mostAncestorInlineContentAtStart) {
    SplitNodeResult splitStartInlineResult = SplitNodeDeepWithTransaction(
        *mostAncestorInlineContentAtStart, aRangeItem.StartPoint(),
        SplitAtEdges::eDoNotCreateEmptyContainer);
    if (splitStartInlineResult.isErr()) {
      NS_WARNING(
          "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
          "eDoNotCreateEmptyContainer) failed");
      return Err(splitStartInlineResult.unwrapErr());
    }
    // XXX Why don't we check editing host like above??
    splitStartInlineResult.MoveCaretPointTo(
        pointToPutCaret, {SuggestCaret::OnlyIfHasSuggestion});
    // XXX If we split only here because of collapsed range, we're modifying
    //     only start point of aRangeItem.  Shouldn't we modify end point here
    //     if it's collapsed?
    const EditorRawDOMPoint& splitPointAtStart =
        splitStartInlineResult.AtSplitPoint<EditorRawDOMPoint>();
    if (MOZ_UNLIKELY(!splitPointAtStart.IsSet())) {
      NS_WARNING(
          "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
          "eDoNotCreateEmptyContainer) didn't return split point");
      return Err(NS_ERROR_FAILURE);
    }
    aRangeItem.mStartContainer = splitPointAtStart.GetContainer();
    aRangeItem.mStartOffset = splitPointAtStart.Offset();
  }

  return pointToPutCaret;
}

Result<EditorDOMPoint, nsresult> HTMLEditor::SplitElementsAtEveryBRElement(
    nsIContent& aMostAncestorToBeSplit,
    nsTArray<OwningNonNull<nsIContent>>& aOutArrayOfContents) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  // First build up a list of all the break nodes inside the inline container.
  AutoTArray<OwningNonNull<HTMLBRElement>, 24> arrayOfBRElements;
  DOMIterator iter(aMostAncestorToBeSplit);
  iter.AppendAllNodesToArray(arrayOfBRElements);

  // If there aren't any breaks, just put inNode itself in the array
  if (arrayOfBRElements.IsEmpty()) {
    aOutArrayOfContents.AppendElement(aMostAncestorToBeSplit);
    return EditorDOMPoint();
  }

  // Else we need to bust up aMostAncestorToBeSplit along all the breaks
  nsCOMPtr<nsIContent> nextContent = &aMostAncestorToBeSplit;
  EditorDOMPoint pointToPutCaret;
  for (OwningNonNull<HTMLBRElement>& brElement : arrayOfBRElements) {
    EditorDOMPoint atBRNode(brElement);
    if (NS_WARN_IF(!atBRNode.IsSet())) {
      return Err(NS_ERROR_FAILURE);
    }
    SplitNodeResult splitNodeResult = SplitNodeDeepWithTransaction(
        *nextContent, atBRNode, SplitAtEdges::eAllowToCreateEmptyContainer);
    if (splitNodeResult.isErr()) {
      NS_WARNING("HTMLEditor::SplitNodeDeepWithTransaction() failed");
      return Err(splitNodeResult.unwrapErr());
    }
    splitNodeResult.MoveCaretPointTo(pointToPutCaret,
                                     {SuggestCaret::OnlyIfHasSuggestion});
    // Put previous node at the split point.
    if (nsIContent* previousContent = splitNodeResult.GetPreviousContent()) {
      // Might not be a left node.  A break might have been at the very
      // beginning of inline container, in which case
      // SplitNodeDeepWithTransaction() would not actually split anything.
      aOutArrayOfContents.AppendElement(*previousContent);
    }

    // Move break outside of container and also put in node list
    // MOZ_KnownLive because 'arrayOfBRElements' is guaranteed to keep it alive.
    MoveNodeResult moveBRElementResult = MoveNodeWithTransaction(
        MOZ_KnownLive(brElement),
        splitNodeResult.AtNextContent<EditorDOMPoint>());
    if (moveBRElementResult.isErr()) {
      NS_WARNING("HTMLEditor::MoveNodeWithTransaction() failed");
      return Err(moveBRElementResult.unwrapErr());
    }
    moveBRElementResult.MoveCaretPointTo(pointToPutCaret,
                                         {SuggestCaret::OnlyIfHasSuggestion});
    aOutArrayOfContents.AppendElement(brElement);

    nextContent = splitNodeResult.GetNextContent();
  }

  // Now tack on remaining next node.
  aOutArrayOfContents.AppendElement(*nextContent);

  return pointToPutCaret;
}

// static
void HTMLEditor::MakeTransitionList(
    const nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents,
    nsTArray<bool>& aTransitionArray) {
  nsINode* prevParent = nullptr;
  aTransitionArray.EnsureLengthAtLeast(aArrayOfContents.Length());
  for (uint32_t i = 0; i < aArrayOfContents.Length(); i++) {
    aTransitionArray[i] = aArrayOfContents[i]->GetParentNode() != prevParent;
    prevParent = aArrayOfContents[i]->GetParentNode();
  }
}

SplitNodeResult HTMLEditor::HandleInsertParagraphInHeadingElement(
    Element& aHeadingElement, const EditorDOMPoint& aPointToSplit) {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  SplitNodeResult splitHeadingResult = [this, &aPointToSplit,
                                        &aHeadingElement]() MOZ_CAN_RUN_SCRIPT {
    // Normalize collapsible white-spaces around the split point to keep
    // them visible after the split.  Note that this does not touch
    // selection because of using AutoTransactionsConserveSelection in
    // WhiteSpaceVisibilityKeeper::ReplaceTextAndRemoveEmptyTextNodes().
    Result<EditorDOMPoint, nsresult> preparationResult =
        WhiteSpaceVisibilityKeeper::PrepareToSplitBlockElement(
            *this, aPointToSplit, aHeadingElement);
    if (MOZ_UNLIKELY(preparationResult.isErr())) {
      NS_WARNING(
          "WhiteSpaceVisibilityKeeper::PrepareToSplitBlockElement() "
          "failed");
      return SplitNodeResult(preparationResult.unwrapErr());
    }
    EditorDOMPoint pointToSplit = preparationResult.unwrap();
    MOZ_ASSERT(pointToSplit.IsInContentNode());

    // Split the header
    SplitNodeResult splitResult = SplitNodeDeepWithTransaction(
        aHeadingElement, pointToSplit,
        SplitAtEdges::eAllowToCreateEmptyContainer);
    NS_WARNING_ASSERTION(
        splitResult.isOk(),
        "HTMLEditor::SplitNodeDeepWithTransaction(aHeadingElement, "
        "SplitAtEdges::eAllowToCreateEmptyContainer) failed");
    return splitResult;
  }();
  if (splitHeadingResult.isErr()) {
    NS_WARNING("Failed to splitting aHeadingElement");
    return splitHeadingResult;
  }
  splitHeadingResult.IgnoreCaretPointSuggestion();
  if (MOZ_UNLIKELY(!splitHeadingResult.DidSplit())) {
    NS_WARNING(
        "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
        "eAllowToCreateEmptyContainer) didn't split aHeadingElement");
    return SplitNodeResult(NS_ERROR_FAILURE);
  }

  // If the left heading element is empty, put a padding <br> element for empty
  // last line into it.
  // FYI: leftHeadingElement is grabbed by splitHeadingResult so that it's safe
  //      to access anytime.
  Element* leftHeadingElement =
      Element::FromNode(splitHeadingResult.GetPreviousContent());
  MOZ_ASSERT(leftHeadingElement,
             "SplitNodeResult::GetPreviousContent() should return something if "
             "DidSplit() returns true");
  MOZ_DIAGNOSTIC_ASSERT(HTMLEditUtils::IsHeader(*leftHeadingElement));
  if (HTMLEditUtils::IsEmptyNode(
          *leftHeadingElement,
          {EmptyCheckOption::TreatSingleBRElementAsVisible})) {
    CreateElementResult insertPaddingBRElementResult =
        InsertPaddingBRElementForEmptyLastLineWithTransaction(
            EditorDOMPoint(leftHeadingElement, 0u));
    if (insertPaddingBRElementResult.isErr()) {
      NS_WARNING(
          "HTMLEditor::InsertPaddingBRElementForEmptyLastLineWithTransaction("
          ") failed");
      return SplitNodeResult(insertPaddingBRElementResult.unwrapErr());
    }
    insertPaddingBRElementResult.IgnoreCaretPointSuggestion();
  }

  // Put caret at start of the right head element if it's not empty.
  Element* rightHeadingElement =
      Element::FromNode(splitHeadingResult.GetNextContent());
  MOZ_ASSERT(rightHeadingElement,
             "SplitNodeResult::GetNextContent() should return something if "
             "DidSplit() returns true");
  if (!HTMLEditUtils::IsEmptyBlockElement(*rightHeadingElement, {})) {
    return SplitNodeResult(std::move(splitHeadingResult),
                           EditorDOMPoint(rightHeadingElement, 0u));
  }

  // If the right heading element is empty, delete it.
  // TODO: If we know the new heading element becomes empty, we stop spliting
  //       the heading element.
  // MOZ_KnownLive(rightHeadingElement) because it's grabbed by
  // splitHeadingResult.
  nsresult rv = DeleteNodeWithTransaction(MOZ_KnownLive(*rightHeadingElement));
  if (NS_FAILED(rv)) {
    NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
    return SplitNodeResult(rv);
  }

  // Layout tells the caret to blink in a weird place if we don't place a
  // break after the header.
  // XXX This block is dead code unless the removed right heading element is
  //     reconnected by a mutation event listener.  This is a regression of
  //     bug 1405751:
  //     https://searchfox.org/mozilla-central/diff/879f3317d1331818718e18776caa47be7f426a22/editor/libeditor/HTMLEditRules.cpp#6389
  //     However, the traditional behavior is different from the other browsers.
  //     Chrome creates new paragraph in this case.  Therefore, we should just
  //     drop this block in a follow up bug.
  if (rightHeadingElement->GetNextSibling()) {
    // XXX Ignoring non-editable <br> element here is odd because non-editable
    //     <br> elements also work as <br> from point of view of layout.
    nsIContent* nextEditableSibling =
        HTMLEditUtils::GetNextSibling(*rightHeadingElement->GetNextSibling(),
                                      {WalkTreeOption::IgnoreNonEditableNode});
    if (nextEditableSibling &&
        nextEditableSibling->IsHTMLElement(nsGkAtoms::br)) {
      auto afterEditableBRElement = EditorDOMPoint::After(*nextEditableSibling);
      if (NS_WARN_IF(!afterEditableBRElement.IsSet())) {
        return SplitNodeResult(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
      }
      // Put caret at the <br> element.
      return SplitNodeResult(
          SplitNodeResult::HandledButDidNotSplitDueToEndOfContainer(
              *leftHeadingElement, GetSplitNodeDirection()),
          afterEditableBRElement);
    }
  }

  if (MOZ_UNLIKELY(!leftHeadingElement->IsInComposedDoc())) {
    NS_WARNING("The left heading element was unexpectedly removed");
    return SplitNodeResult(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  TopLevelEditSubActionDataRef().mCachedInlineStyles->Clear();
  mTypeInState->ClearAllProps();

  // Create a paragraph if the right heading element is not followed by an
  // editable <br> element.
  nsStaticAtom& newParagraphTagName =
      &DefaultParagraphSeparatorTagName() == nsGkAtoms::br
          ? *nsGkAtoms::p
          : DefaultParagraphSeparatorTagName();
  // We want a wrapper element even if we separate with a <br>.
  // MOZ_KnownLive(newParagraphTagName) because it's available until shutdown.
  const CreateElementResult createNewParagraphElementResult =
      CreateAndInsertElement(
          WithTransaction::Yes, MOZ_KnownLive(newParagraphTagName),
          EditorDOMPoint::After(*leftHeadingElement),
          // MOZ_CAN_RUN_SCRIPT_BOUNDARY due to bug 1758868
          [](HTMLEditor& aHTMLEditor, Element& aDivOrParagraphElement,
             const EditorDOMPoint&) MOZ_CAN_RUN_SCRIPT_BOUNDARY {
            // We don't make inserting new <br> element undoable because
            // removing the new element from the DOM tree gets same result for
            // the user if aDivOrParagraphElement has not been connected yet.
            const auto withTransaction =
                aDivOrParagraphElement.IsInComposedDoc() ? WithTransaction::Yes
                                                         : WithTransaction::No;
            CreateElementResult insertBRElementResult =
                aHTMLEditor.InsertBRElement(
                    withTransaction,
                    EditorDOMPoint(&aDivOrParagraphElement, 0u));
            if (insertBRElementResult.isErr()) {
              NS_WARNING(
                  nsPrintfCString("HTMLEditor::InsertBRElement(%s) failed",
                                  ToString(withTransaction).c_str())
                      .get());
              return insertBRElementResult.unwrapErr();
            }
            // We'll update selection after inserting the new paragraph.
            insertBRElementResult.IgnoreCaretPointSuggestion();
            return NS_OK;
          });
  if (createNewParagraphElementResult.isErr()) {
    NS_WARNING(
        "HTMLEditor::CreateAndInsertElement(WithTransaction::Yes) failed");
    return SplitNodeResult(createNewParagraphElementResult.unwrapErr());
  }
  // Put caret at the <br> element in the following paragraph.
  createNewParagraphElementResult.IgnoreCaretPointSuggestion();
  MOZ_ASSERT(createNewParagraphElementResult.GetNewNode());
  return SplitNodeResult(
      *leftHeadingElement, *createNewParagraphElementResult.GetNewNode(),
      GetSplitNodeDirection(),
      Some(EditorDOMPoint(createNewParagraphElementResult.GetNewNode(), 0u)));
}

SplitNodeResult HTMLEditor::HandleInsertParagraphInParagraph(
    Element& aParentDivOrP, const EditorDOMPoint& aCandidatePointToSplit,
    const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aCandidatePointToSplit.IsSetAndValid());

  // First, get a better split point to avoid to create a new empty link in the
  // right paragraph.
  EditorDOMPoint pointToSplit = [&]() {
    // We shouldn't create new anchor element which has non-empty href unless
    // splitting middle of it because we assume that users don't want to create
    // *same* anchor element across two or more paragraphs in most cases.
    // So, adjust selection start if it's edge of anchor element(s).
    // XXX We don't support white-space collapsing in these cases since it needs
    //     some additional work with WhiteSpaceVisibilityKeeper but it's not
    //     usual case. E.g., |<a href="foo"><b>foo []</b> </a>|
    if (aCandidatePointToSplit.IsStartOfContainer()) {
      EditorDOMPoint candidatePoint(aCandidatePointToSplit);
      for (nsIContent* container =
               aCandidatePointToSplit.GetContainerAs<nsIContent>();
           container && container != &aParentDivOrP;
           container = container->GetParent()) {
        if (HTMLEditUtils::IsLink(container)) {
          // Found link should be only in right node.  So, we shouldn't split
          // it.
          candidatePoint.Set(container);
          // Even if we found an anchor element, don't break because DOM API
          // allows to nest anchor elements.
        }
        // If the container is middle of its parent, stop adjusting split point.
        if (container->GetPreviousSibling()) {
          // XXX Should we check if previous sibling is visible content?
          //     E.g., should we ignore comment node, invisible <br> element?
          break;
        }
      }
      return candidatePoint;
    }

    // We also need to check if selection is at invisible <br> element at end
    // of an <a href="foo"> element because editor inserts a <br> element when
    // user types Enter key after a white-space which is at middle of
    // <a href="foo"> element and when setting selection at end of the element,
    // selection becomes referring the <br> element.  We may need to change this
    // behavior later if it'd be standardized.
    if (aCandidatePointToSplit.IsEndOfContainer() ||
        aCandidatePointToSplit.IsBRElementAtEndOfContainer()) {
      // If there are 2 <br> elements, the first <br> element is visible.  E.g.,
      // |<a href="foo"><b>boo[]<br></b><br></a>|, we should split the <a>
      // element.  Otherwise, E.g., |<a href="foo"><b>boo[]<br></b></a>|,
      // we should not split the <a> element and ignore inline elements in it.
      bool foundBRElement =
          aCandidatePointToSplit.IsBRElementAtEndOfContainer();
      EditorDOMPoint candidatePoint(aCandidatePointToSplit);
      for (nsIContent* container =
               aCandidatePointToSplit.GetContainerAs<nsIContent>();
           container && container != &aParentDivOrP;
           container = container->GetParent()) {
        if (HTMLEditUtils::IsLink(container)) {
          // Found link should be only in left node.  So, we shouldn't split it.
          candidatePoint.SetAfter(container);
          // Even if we found an anchor element, don't break because DOM API
          // allows to nest anchor elements.
        }
        // If the container is middle of its parent, stop adjusting split point.
        if (nsIContent* nextSibling = container->GetNextSibling()) {
          if (foundBRElement) {
            // If we've already found a <br> element, we assume found node is
            // visible <br> or something other node.
            // XXX Should we check if non-text data node like comment?
            break;
          }

          // XXX Should we check if non-text data node like comment?
          if (!nextSibling->IsHTMLElement(nsGkAtoms::br)) {
            break;
          }
          foundBRElement = true;
        }
      }
      return candidatePoint;
    }
    return aCandidatePointToSplit;
  }();

  const bool createNewParagraph = GetReturnInParagraphCreatesNewParagraph();
  RefPtr<HTMLBRElement> brElement;
  if (createNewParagraph && pointToSplit.GetContainer() == &aParentDivOrP) {
    // We are try to split only the current paragraph.  Therefore, we don't need
    // to create new <br> elements around it (if left and/or right paragraph
    // becomes empty, it'll be treated by SplitParagraphWithTransaction().
    brElement = nullptr;
  } else if (pointToSplit.IsInTextNode()) {
    if (pointToSplit.IsStartOfContainer()) {
      // If we're splitting the paragraph at start of a text node and there is
      // no preceding visible <br> element, we need to create a <br> element to
      // keep the inline elements containing this text node.
      // TODO: If the parent of the text node is the splitting paragraph,
      //       obviously we don't need to do this because empty paragraphs will
      //       be treated by SplitParagraphWithTransaction().  In this case, we
      //       just need to update pointToSplit for using the same path as the
      //       previous `if` block.
      brElement =
          HTMLBRElement::FromNodeOrNull(HTMLEditUtils::GetPreviousSibling(
              *pointToSplit.ContainerAs<Text>(),
              {WalkTreeOption::IgnoreNonEditableNode}));
      if (!brElement || HTMLEditUtils::IsInvisibleBRElement(*brElement) ||
          EditorUtils::IsPaddingBRElementForEmptyLastLine(*brElement)) {
        // If insertParagraph does not create a new paragraph, default to
        // insertLineBreak.
        if (!createNewParagraph) {
          return SplitNodeResult::NotHandled(pointToSplit,
                                             GetSplitNodeDirection());
        }
        const EditorDOMPoint pointToInsertBR = pointToSplit.ParentPoint();
        MOZ_ASSERT(pointToInsertBR.IsSet());
        CreateElementResult insertBRElementResult =
            InsertBRElement(WithTransaction::Yes, pointToInsertBR);
        if (insertBRElementResult.isErr()) {
          NS_WARNING(
              "HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
          return SplitNodeResult(insertBRElementResult.unwrapErr());
        }
        // We'll collapse `Selection` to the place suggested by
        // SplitParagraphWithTransaction.
        insertBRElementResult.IgnoreCaretPointSuggestion();
        brElement =
            HTMLBRElement::FromNodeOrNull(insertBRElementResult.GetNewNode());
      }
    } else if (pointToSplit.IsEndOfContainer()) {
      // If we're splitting the paragraph at end of a text node and there is not
      // following visible <br> element, we need to create a <br> element after
      // the text node to make current style specified by parent inline elements
      // keep in the right paragraph.
      // TODO: Same as above, we don't need to do this if the text node is a
      //       direct child of the paragraph.  For using the simplest path, we
      //       just need to update `pointToSplit` in the case.
      brElement = HTMLBRElement::FromNodeOrNull(HTMLEditUtils::GetNextSibling(
          *pointToSplit.ContainerAs<Text>(),
          {WalkTreeOption::IgnoreNonEditableNode}));
      if (!brElement || HTMLEditUtils::IsInvisibleBRElement(*brElement) ||
          EditorUtils::IsPaddingBRElementForEmptyLastLine(*brElement)) {
        // If insertParagraph does not create a new paragraph, default to
        // insertLineBreak.
        if (!createNewParagraph) {
          return SplitNodeResult::NotHandled(pointToSplit,
                                             GetSplitNodeDirection());
        }
        const EditorDOMPoint pointToInsertBR =
            EditorDOMPoint::After(*pointToSplit.ContainerAs<Text>());
        MOZ_ASSERT(pointToInsertBR.IsSet());
        CreateElementResult insertBRElementResult =
            InsertBRElement(WithTransaction::Yes, pointToInsertBR);
        if (insertBRElementResult.isErr()) {
          NS_WARNING(
              "HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
          return SplitNodeResult(insertBRElementResult.unwrapErr());
        }
        // We'll collapse `Selection` to the place suggested by
        // SplitParagraphWithTransaction.
        insertBRElementResult.IgnoreCaretPointSuggestion();
        brElement =
            HTMLBRElement::FromNodeOrNull(insertBRElementResult.GetNewNode());
      }
    } else {
      // If insertParagraph does not create a new paragraph, default to
      // insertLineBreak.
      if (!createNewParagraph) {
        return SplitNodeResult::NotHandled(pointToSplit,
                                           GetSplitNodeDirection());
      }

      // If we're splitting the paragraph at middle of a text node, we should
      // split the text node here and put a <br> element next to the left text
      // node.
      // XXX Why? I think that this should be handled in
      //     SplitParagraphWithTransaction() directly because I don't find
      //     the necessary case of the <br> element.

      // XXX We split a text node here if caret is middle of it to insert
      //     <br> element **before** splitting aParentDivOrP.  Then, if
      //     the <br> element becomes unnecessary, it'll be removed again.
      //     So this does much more complicated things than what we want to
      //     do here.  We should handle this case separately to make the code
      //     much simpler.

      // Normalize collapsible white-spaces around the split point to keep
      // them visible after the split.  Note that this does not touch
      // selection because of using AutoTransactionsConserveSelection in
      // WhiteSpaceVisibilityKeeper::ReplaceTextAndRemoveEmptyTextNodes().
      Result<EditorDOMPoint, nsresult> pointToSplitOrError =
          WhiteSpaceVisibilityKeeper::PrepareToSplitBlockElement(
              *this, pointToSplit, aParentDivOrP);
      if (NS_WARN_IF(Destroyed())) {
        return SplitNodeResult(NS_ERROR_EDITOR_DESTROYED);
      }
      if (MOZ_UNLIKELY(pointToSplitOrError.isErr())) {
        NS_WARNING(
            "WhiteSpaceVisibilityKeeper::PrepareToSplitBlockElement() "
            "failed");
        return SplitNodeResult(pointToSplitOrError.unwrapErr());
      }
      MOZ_ASSERT(pointToSplitOrError.inspect().IsSetAndValid());
      if (pointToSplitOrError.inspect().IsSet()) {
        pointToSplit = pointToSplitOrError.unwrap();
      }
      SplitNodeResult splitParentDivOrPResult =
          SplitNodeWithTransaction(pointToSplit);
      if (splitParentDivOrPResult.isErr()) {
        NS_WARNING("HTMLEditor::SplitNodeWithTransaction() failed");
        return splitParentDivOrPResult;
      }
      // We'll collapse `Selection` to the place suggested by
      // SplitParagraphWithTransaction.
      splitParentDivOrPResult.IgnoreCaretPointSuggestion();

      pointToSplit.SetToEndOf(splitParentDivOrPResult.GetPreviousContent());
      if (NS_WARN_IF(!pointToSplit.IsInContentNode())) {
        return SplitNodeResult(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
      }

      // We need to put new <br> after the left node if given node was split
      // above.
      const EditorDOMPoint pointToInsertBR =
          EditorDOMPoint::After(*pointToSplit.ContainerAs<nsIContent>());
      MOZ_ASSERT(pointToInsertBR.IsSet());
      CreateElementResult insertBRElementResult =
          InsertBRElement(WithTransaction::Yes, pointToInsertBR);
      if (insertBRElementResult.isErr()) {
        NS_WARNING("HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
        return SplitNodeResult(insertBRElementResult.unwrapErr());
      }
      // We'll collapse `Selection` to the place suggested by
      // SplitParagraphWithTransaction.
      insertBRElementResult.IgnoreCaretPointSuggestion();
      brElement =
          HTMLBRElement::FromNodeOrNull(insertBRElementResult.GetNewNode());
    }
  } else {
    // If we're splitting in a child element of the paragraph, and there is no
    // <br> element around it, we should insert a <br> element at the split
    // point and keep splitting the paragraph after the new <br> element.
    // XXX Why? We probably need to do this if we're splitting in an inline
    //     element which and whose parents provide some styles, we should put
    //     the <br> element for making a placeholder in the left paragraph for
    //     moving to the caret, but I think that this could be handled in fewer
    //     cases than this.
    brElement = HTMLBRElement::FromNodeOrNull(HTMLEditUtils::GetPreviousContent(
        pointToSplit, {WalkTreeOption::IgnoreNonEditableNode}, &aEditingHost));
    if (!brElement || HTMLEditUtils::IsInvisibleBRElement(*brElement) ||
        EditorUtils::IsPaddingBRElementForEmptyLastLine(*brElement)) {
      // is there a BR after it?
      brElement = HTMLBRElement::FromNodeOrNull(HTMLEditUtils::GetNextContent(
          pointToSplit, {WalkTreeOption::IgnoreNonEditableNode},
          &aEditingHost));
      if (!brElement || HTMLEditUtils::IsInvisibleBRElement(*brElement) ||
          EditorUtils::IsPaddingBRElementForEmptyLastLine(*brElement)) {
        // If insertParagraph does not create a new paragraph, default to
        // insertLineBreak.
        if (!createNewParagraph) {
          return SplitNodeResult::NotHandled(pointToSplit,
                                             GetSplitNodeDirection());
        }
        CreateElementResult insertBRElementResult =
            InsertBRElement(WithTransaction::Yes, pointToSplit);
        if (insertBRElementResult.isErr()) {
          NS_WARNING(
              "HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
          return SplitNodeResult(insertBRElementResult.unwrapErr());
        }
        // We'll collapse `Selection` to the place suggested by
        // SplitParagraphWithTransaction.
        insertBRElementResult.IgnoreCaretPointSuggestion();
        brElement =
            HTMLBRElement::FromNodeOrNull(insertBRElementResult.GetNewNode());
        // We split the parent after the <br>.
        pointToSplit.SetAfter(brElement);
        if (NS_WARN_IF(!pointToSplit.IsSet())) {
          return SplitNodeResult(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
        }
      }
    }
  }

  SplitNodeResult splitParagraphResult =
      SplitParagraphWithTransaction(aParentDivOrP, pointToSplit, brElement);
  if (splitParagraphResult.isErr()) {
    NS_WARNING("HTMLEditor::SplitParagraphWithTransaction() failed");
    return splitParagraphResult;
  }
  if (MOZ_UNLIKELY(!splitParagraphResult.DidSplit())) {
    NS_WARNING(
        "HTMLEditor::SplitParagraphWithTransaction() didn't split the "
        "paragraph");
    splitParagraphResult.IgnoreCaretPointSuggestion();
    return SplitNodeResult(NS_ERROR_FAILURE);
  }
  MOZ_ASSERT(splitParagraphResult.Handled());
  return splitParagraphResult;
}

SplitNodeResult HTMLEditor::SplitParagraphWithTransaction(
    Element& aParentDivOrP, const EditorDOMPoint& aStartOfRightNode,
    HTMLBRElement* aMayBecomeVisibleBRElement) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  Result<EditorDOMPoint, nsresult> preparationResult =
      WhiteSpaceVisibilityKeeper::PrepareToSplitBlockElement(
          *this, aStartOfRightNode, aParentDivOrP);
  if (MOZ_UNLIKELY(preparationResult.isErr())) {
    NS_WARNING(
        "WhiteSpaceVisibilityKeeper::PrepareToSplitBlockElement() failed");
    return SplitNodeResult(preparationResult.unwrapErr());
  }
  EditorDOMPoint pointToSplit = preparationResult.unwrap();
  MOZ_ASSERT(pointToSplit.IsInContentNode());

  // Split the paragraph.
  SplitNodeResult splitDivOrPResult = SplitNodeDeepWithTransaction(
      aParentDivOrP, pointToSplit, SplitAtEdges::eAllowToCreateEmptyContainer);
  if (splitDivOrPResult.isErr()) {
    NS_WARNING("HTMLEditor::SplitNodeDeepWithTransaction() failed");
    return splitDivOrPResult;
  }
  if (MOZ_UNLIKELY(!splitDivOrPResult.DidSplit())) {
    NS_WARNING(
        "HTMLEditor::SplitNodeDeepWithTransaction() didn't split any nodes");
    return splitDivOrPResult;
  }

  // We'll compute caret suggestion later.  So the simple result is not needed.
  splitDivOrPResult.IgnoreCaretPointSuggestion();

  Element* leftDivOrParagraphElement =
      Element::FromNode(splitDivOrPResult.GetPreviousContent());
  MOZ_ASSERT(leftDivOrParagraphElement,
             "SplitNodeResult::GetPreviousContent() should return something if "
             "DidSplit() returns true");
  Element* rightDivOrParagraphElement =
      Element::FromNode(splitDivOrPResult.GetNextContent());
  MOZ_ASSERT(rightDivOrParagraphElement,
             "SplitNodeResult::GetNextContent() should return something if "
             "DidSplit() returns true");

  // Get rid of the break, if it is visible (otherwise it may be needed to
  // prevent an empty p).
  if (aMayBecomeVisibleBRElement &&
      HTMLEditUtils::IsVisibleBRElement(*aMayBecomeVisibleBRElement)) {
    nsresult rv = DeleteNodeWithTransaction(*aMayBecomeVisibleBRElement);
    if (NS_FAILED(rv)) {
      NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
      return SplitNodeResult(rv);
    }
  }

  // Remove ID attribute on the paragraph from the right node.
  // MOZ_KnownLive(rightDivOrParagraphElement) because it's grabbed by
  // splitDivOrPResult.
  nsresult rv = RemoveAttributeWithTransaction(
      MOZ_KnownLive(*rightDivOrParagraphElement), *nsGkAtoms::id);
  if (NS_WARN_IF(Destroyed())) {
    return SplitNodeResult(NS_ERROR_EDITOR_DESTROYED);
  }
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "EditorBase::RemoveAttributeWithTransaction(nsGkAtoms::id) failed");
    return SplitNodeResult(rv);
  }

  // We need to ensure to both paragraphs visible even if they are empty.
  // However, padding <br> element for empty last line isn't useful in this
  // case because it'll be ignored by PlaintextSerializer.  Additionally,
  // it'll be exposed as <br> with Element.innerHTML.  Therefore, we can use
  // normal <br> elements for placeholder in this case.  Note that Chromium
  // also behaves so.
  auto InsertBRElementIfEmptyBlockElement =
      [&](Element& aElement) MOZ_CAN_RUN_SCRIPT {
        if (!HTMLEditUtils::IsBlockElement(aElement)) {
          return NS_OK;
        }

        if (!HTMLEditUtils::IsEmptyNode(
                aElement, {EmptyCheckOption::TreatSingleBRElementAsVisible})) {
          return NS_OK;
        }

        // XXX: Probably, we should use
        //      InsertPaddingBRElementForEmptyLastLineWithTransaction here, and
        //      if there are some empty inline container, we should put the <br>
        //      into the last one.
        CreateElementResult insertBRElementResult = InsertBRElement(
            WithTransaction::Yes, EditorDOMPoint(&aElement, 0u));
        if (insertBRElementResult.isErr()) {
          NS_WARNING(
              "HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
          return insertBRElementResult.unwrapErr();
        }
        // After this is called twice, we'll compute new caret position.
        // Therefore, we don't need to update selection here.
        insertBRElementResult.IgnoreCaretPointSuggestion();
        return NS_OK;
      };

  // MOZ_KnownLive(leftDivOrParagraphElement) because it's grabbed by
  // splitDivOrResult.
  rv = InsertBRElementIfEmptyBlockElement(
      MOZ_KnownLive(*leftDivOrParagraphElement));
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "InsertBRElementIfEmptyBlockElement(leftDivOrParagraphElement) failed");
    return SplitNodeResult(rv);
  }

  if (HTMLEditUtils::IsEmptyNode(*rightDivOrParagraphElement)) {
    // If the right paragraph is empty, it might have an empty inline element
    // (which may contain other empty inline containers) and optionally a <br>
    // element which may not be in the deepest inline element.
    const RefPtr<Element> deepestInlineContainerElement =
        [](const Element& aBlockElement) {
          Element* result = nullptr;
          for (Element* maybeDeepestInlineContainer =
                   Element::FromNodeOrNull(aBlockElement.GetFirstChild());
               maybeDeepestInlineContainer &&
               HTMLEditUtils::IsInlineElement(*maybeDeepestInlineContainer) &&
               HTMLEditUtils::IsContainerNode(*maybeDeepestInlineContainer);
               maybeDeepestInlineContainer =
                   maybeDeepestInlineContainer->GetFirstElementChild()) {
            result = maybeDeepestInlineContainer;
          }
          return result;
        }(*rightDivOrParagraphElement);
    if (deepestInlineContainerElement) {
      RefPtr<HTMLBRElement> brElement =
          HTMLEditUtils::GetFirstBRElement(*rightDivOrParagraphElement);
      // If there is a <br> element and it is in the deepest inline container,
      // we need to do nothing anymore. Let's suggest caret position as at the
      // <br>.
      if (brElement &&
          brElement->GetParentNode() == deepestInlineContainerElement) {
        brElement->SetFlags(NS_PADDING_FOR_EMPTY_LAST_LINE);
        return SplitNodeResult(std::move(splitDivOrPResult),
                               EditorDOMPoint(brElement));
      }
      // Otherwise, we should put a padding <br> element into the deepest inline
      // container and then, existing <br> element (if there is) becomes
      // unnecessary.
      if (brElement) {
        nsresult rv = DeleteNodeWithTransaction(*brElement);
        if (NS_FAILED(rv)) {
          NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
          return SplitNodeResult(rv);
        }
      }
      const CreateElementResult insertPaddingBRElementResult =
          InsertPaddingBRElementForEmptyLastLineWithTransaction(
              EditorDOMPoint::AtEndOf(deepestInlineContainerElement));
      if (insertPaddingBRElementResult.isErr()) {
        NS_WARNING(
            "HTMLEditor::"
            "InsertPaddingBRElementForEmptyLastLineWithTransaction() failed");
        return SplitNodeResult(insertPaddingBRElementResult.inspectErr());
      }
      insertPaddingBRElementResult.IgnoreCaretPointSuggestion();
      return SplitNodeResult(
          std::move(splitDivOrPResult),
          EditorDOMPoint(insertPaddingBRElementResult.GetNewNode()));
    }

    // If there is no inline container elements, we just need to make the
    // right paragraph visible.
    nsresult rv = InsertBRElementIfEmptyBlockElement(
        MOZ_KnownLive(*rightDivOrParagraphElement));
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "InsertBRElementIfEmptyBlockElement(rightDivOrParagraphElement) "
          "failed");
      return SplitNodeResult(rv);
    }
  }

  // Let's put caret at start of the first leaf container.
  nsIContent* child = HTMLEditUtils::GetFirstLeafContent(
      *rightDivOrParagraphElement, {LeafNodeType::LeafNodeOrChildBlock});
  if (MOZ_UNLIKELY(!child)) {
    return SplitNodeResult(std::move(splitDivOrPResult),
                           EditorDOMPoint(rightDivOrParagraphElement, 0u));
  }
  return child->IsText() || HTMLEditUtils::IsContainerNode(*child)
             ? SplitNodeResult(std::move(splitDivOrPResult),
                               EditorDOMPoint(child, 0u))
             : SplitNodeResult(std::move(splitDivOrPResult),
                               EditorDOMPoint(child));
}

Result<EditorDOMPoint, nsresult>
HTMLEditor::HandleInsertParagraphInListItemElement(
    Element& aListItemElement, const EditorDOMPoint& aPointToSplit,
    const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(HTMLEditUtils::IsListItem(&aListItemElement));

  // If aListItemElement is empty, then we want to outdent its content.
  if (&aEditingHost != aListItemElement.GetParentElement() &&
      HTMLEditUtils::IsEmptyBlockElement(aListItemElement, {})) {
    RefPtr<Element> leftListElement = aListItemElement.GetParentElement();
    // If the given list item element is not the last list item element of
    // its parent nor not followed by sub list elements, split the parent
    // before it.
    if (!HTMLEditUtils::IsLastChild(aListItemElement,
                                    {WalkTreeOption::IgnoreNonEditableNode})) {
      const SplitNodeResult splitListItemParentResult =
          SplitNodeWithTransaction(EditorDOMPoint(&aListItemElement));
      if (splitListItemParentResult.isErr()) {
        NS_WARNING("HTMLEditor::SplitNodeWithTransaction() failed");
        return Err(splitListItemParentResult.unwrapErr());
      }
      if (MOZ_UNLIKELY(!splitListItemParentResult.DidSplit())) {
        NS_WARNING(
            "HTMLEditor::SplitNodeWithTransaction() didn't split the parent of "
            "aListItemElement");
        MOZ_ASSERT(!splitListItemParentResult.HasCaretPointSuggestion());
        return Err(NS_ERROR_FAILURE);
      }
      splitListItemParentResult.IgnoreCaretPointSuggestion();
      leftListElement =
          Element::FromNode(splitListItemParentResult.GetPreviousContent());
      MOZ_DIAGNOSTIC_ASSERT(leftListElement);
    }

    auto afterLeftListElement = EditorDOMPoint::After(leftListElement);
    if (MOZ_UNLIKELY(!afterLeftListElement.IsSet())) {
      return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
    }

    // If aListItemElement is in an invalid sub-list element, move it into
    // the grand parent list element in order to outdent.
    if (HTMLEditUtils::IsAnyListElement(afterLeftListElement.GetContainer())) {
      const MoveNodeResult moveListItemElementResult =
          MoveNodeWithTransaction(aListItemElement, afterLeftListElement);
      if (moveListItemElementResult.isErr()) {
        NS_WARNING("HTMLEditor::MoveNodeWithTransaction() failed");
        return Err(moveListItemElementResult.unwrapErr());
      }
      moveListItemElementResult.IgnoreCaretPointSuggestion();
      return EditorDOMPoint(&aListItemElement, 0u);
    }

    // Otherwise, replace the empty aListItemElement with a new paragraph.
    nsresult rv = DeleteNodeWithTransaction(aListItemElement);
    if (NS_FAILED(rv)) {
      NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
      return Err(rv);
    }
    nsStaticAtom& newParagraphTagName =
        &DefaultParagraphSeparatorTagName() == nsGkAtoms::br
            ? *nsGkAtoms::p
            : DefaultParagraphSeparatorTagName();
    // MOZ_KnownLive(newParagraphTagName) because it's available until shutdown.
    const CreateElementResult createNewParagraphElementResult =
        CreateAndInsertElement(
            WithTransaction::Yes, MOZ_KnownLive(newParagraphTagName),
            afterLeftListElement,
            // MOZ_CAN_RUN_SCRIPT_BOUNDARY due to bug 1758868
            [](HTMLEditor& aHTMLEditor, Element& aDivOrParagraphElement,
               const EditorDOMPoint&) MOZ_CAN_RUN_SCRIPT_BOUNDARY {
              // We don't make inserting new <br> element undoable because
              // removing the new element from the DOM tree gets same result
              // for the user if aDivOrParagraphElement has not been
              // connected yet.
              const auto withTransaction =
                  aDivOrParagraphElement.IsInComposedDoc()
                      ? WithTransaction::Yes
                      : WithTransaction::No;
              CreateElementResult insertBRElementResult =
                  aHTMLEditor.InsertBRElement(
                      withTransaction,
                      EditorDOMPoint(&aDivOrParagraphElement, 0u));
              if (insertBRElementResult.isErr()) {
                NS_WARNING(
                    nsPrintfCString("HTMLEditor::InsertBRElement(%s) failed",
                                    ToString(withTransaction).c_str())
                        .get());
                return insertBRElementResult.unwrapErr();
              }
              // We'll update selection after inserting the paragraph.
              insertBRElementResult.IgnoreCaretPointSuggestion();
              return NS_OK;
            });
    if (createNewParagraphElementResult.isErr()) {
      NS_WARNING(
          "HTMLEditor::CreateAndInsertElement(WithTransaction::Yes) failed");
      return Err(createNewParagraphElementResult.unwrapErr());
    }
    createNewParagraphElementResult.IgnoreCaretPointSuggestion();
    MOZ_ASSERT(createNewParagraphElementResult.GetNewNode());
    return EditorDOMPoint(createNewParagraphElementResult.GetNewNode(), 0u);
  }

  // If aListItemElement has some content or aListItemElement is empty but it's
  // a child of editing host, we want a new list item at the same list level.
  // First, sort out white-spaces.
  Result<EditorDOMPoint, nsresult> preparationResult =
      WhiteSpaceVisibilityKeeper::PrepareToSplitBlockElement(
          *this, aPointToSplit, aListItemElement);
  if (preparationResult.isErr()) {
    NS_WARNING(
        "WhiteSpaceVisibilityKeeper::PrepareToSplitBlockElement() failed");
    return Err(preparationResult.unwrapErr());
  }
  EditorDOMPoint pointToSplit = preparationResult.unwrap();
  MOZ_ASSERT(pointToSplit.IsInContentNode());

  // Now split the list item.
  const SplitNodeResult splitListItemResult =
      SplitNodeDeepWithTransaction(aListItemElement, pointToSplit,
                                   SplitAtEdges::eAllowToCreateEmptyContainer);
  if (splitListItemResult.isErr()) {
    NS_WARNING("HTMLEditor::SplitNodeDeepWithTransaction() failed");
    return Err(splitListItemResult.unwrapErr());
  }
  splitListItemResult.IgnoreCaretPointSuggestion();
  if (MOZ_UNLIKELY(!aListItemElement.GetParent())) {
    NS_WARNING("Somebody disconnected the target listitem from the parent");
    return Err(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  // If aListItemElement is not replaced, we should not do anything anymore.
  if (MOZ_UNLIKELY(!splitListItemResult.DidSplit()) ||
      NS_WARN_IF(!splitListItemResult.GetNewContent()->IsElement()) ||
      NS_WARN_IF(!splitListItemResult.GetOriginalContent()->IsElement())) {
    NS_WARNING("HTMLEditor::SplitNodeDeepWithTransaction() didn't split");
    return Err(NS_ERROR_FAILURE);
  }

  // FYI: They are grabbed by splitListItemResult so that they are known live
  //      things.
  Element& leftListItemElement =
      *splitListItemResult.GetPreviousContent()->AsElement();
  Element& rightListItemElement =
      *splitListItemResult.GetNextContent()->AsElement();

  // Hack: until I can change the damaged doc range code back to being
  // extra-inclusive, I have to manually detect certain list items that may be
  // left empty.
  if (HTMLEditUtils::IsEmptyNode(
          leftListItemElement,
          {EmptyCheckOption::TreatSingleBRElementAsVisible})) {
    CreateElementResult insertPaddingBRElementResult =
        InsertPaddingBRElementForEmptyLastLineWithTransaction(
            EditorDOMPoint(&leftListItemElement, 0u));
    if (insertPaddingBRElementResult.isErr()) {
      NS_WARNING(
          "HTMLEditor::InsertPaddingBRElementForEmptyLastLineWithTransaction("
          ") failed");
      return Err(insertPaddingBRElementResult.unwrapErr());
    }
    // We're returning a candidate point to put caret so that we don't need to
    // update now.
    insertPaddingBRElementResult.IgnoreCaretPointSuggestion();
    return EditorDOMPoint(&rightListItemElement, 0u);
  }

  if (HTMLEditUtils::IsEmptyNode(rightListItemElement)) {
    // If aListItemElement is a <dd> or a <dt> and the right list item is empty
    // or a direct child of the editing host, replace it a new list item element
    // whose type is the other one.
    if (aListItemElement.IsAnyOfHTMLElements(nsGkAtoms::dd, nsGkAtoms::dt)) {
      nsStaticAtom& nextDefinitionListItemTagName =
          aListItemElement.IsHTMLElement(nsGkAtoms::dt) ? *nsGkAtoms::dd
                                                        : *nsGkAtoms::dt;
      // MOZ_KnownLive(nextDefinitionListItemTagName) because it's available
      // until shutdown.
      CreateElementResult createNewListItemElementResult =
          CreateAndInsertElement(WithTransaction::Yes,
                                 MOZ_KnownLive(nextDefinitionListItemTagName),
                                 EditorDOMPoint::After(rightListItemElement));
      if (createNewListItemElementResult.isErr()) {
        NS_WARNING(
            "HTMLEditor::CreateAndInsertElement(WithTransaction::Yes) failed");
        return Err(createNewListItemElementResult.unwrapErr());
      }
      createNewListItemElementResult.IgnoreCaretPointSuggestion();
      RefPtr<Element> newListItemElement =
          createNewListItemElementResult.UnwrapNewNode();
      MOZ_ASSERT(newListItemElement);
      // MOZ_KnownLive(rightListItemElement) because it's grabbed by
      // splitListItemResult.
      nsresult rv =
          DeleteNodeWithTransaction(MOZ_KnownLive(rightListItemElement));
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
        return Err(rv);
      }
      return EditorDOMPoint(newListItemElement, 0u);
    }

    // If aListItemElement is a <li> and the right list item becomes empty or a
    // direct child of the editing host, copy all inline elements affecting to
    // the style at end of the left list item element to the right list item
    // element.
    // MOZ_KnownLive(*ListItemElement) because they are grabbed by
    // splitListItemResult.
    Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
        CopyLastEditableChildStylesWithTransaction(
            MOZ_KnownLive(leftListItemElement),
            MOZ_KnownLive(rightListItemElement), aEditingHost);
    if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
      NS_WARNING(
          "HTMLEditor::CopyLastEditableChildStylesWithTransaction() failed");
      return Err(pointToPutCaretOrError.unwrapErr());
    }
    return pointToPutCaretOrError.unwrap();
  }

  // If the right list item element is not empty, we need to consider where to
  // put caret in it. If it has non-container inline elements, <br> or <hr>, at
  // the element is proper position.
  WSScanResult forwardScanFromStartOfListItemResult =
      WSRunScanner::ScanNextVisibleNodeOrBlockBoundary(
          &aEditingHost, EditorRawDOMPoint(&rightListItemElement, 0u));
  if (MOZ_UNLIKELY(forwardScanFromStartOfListItemResult.Failed())) {
    NS_WARNING("WSRunScanner::ScanNextVisibleNodeOrBlockBoundary() failed");
    return Err(NS_ERROR_FAILURE);
  }
  if (forwardScanFromStartOfListItemResult.ReachedSpecialContent() ||
      forwardScanFromStartOfListItemResult.ReachedBRElement() ||
      forwardScanFromStartOfListItemResult.ReachedHRElement()) {
    auto atFoundElement =
        forwardScanFromStartOfListItemResult.PointAtContent<EditorDOMPoint>();
    if (NS_WARN_IF(!atFoundElement.IsSetAndValid())) {
      return Err(NS_ERROR_FAILURE);
    }
    return atFoundElement;
  }

  // Otherwise, return the point at first visible thing.
  // XXX This may be not meaningful position if it reached block element
  //     in aListItemElement.
  return forwardScanFromStartOfListItemResult.Point<EditorDOMPoint>();
}

CreateElementResult HTMLEditor::WrapContentsInBlockquoteElementsWithTransaction(
    const nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents,
    const Element& aEditingHost) {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  // The idea here is to put the nodes into a minimal number of blockquotes.
  // When the user blockquotes something, they expect one blockquote.  That
  // may not be possible (for instance, if they have two table cells selected,
  // you need two blockquotes inside the cells).
  RefPtr<Element> curBlock, blockElementToPutCaret;
  nsCOMPtr<nsINode> prevParent;

  EditorDOMPoint pointToPutCaret;
  for (auto& content : aArrayOfContents) {
    // If the node is a table element or list item, dive inside
    if (HTMLEditUtils::IsAnyTableElementButNotTable(content) ||
        HTMLEditUtils::IsListItem(content)) {
      // Forget any previous block
      curBlock = nullptr;
      // Recursion time
      AutoTArray<OwningNonNull<nsIContent>, 24> childContents;
      HTMLEditor::GetChildNodesOf(*content, childContents);
      CreateElementResult wrapChildrenInAnotherBlockquoteResult =
          WrapContentsInBlockquoteElementsWithTransaction(childContents,
                                                          aEditingHost);
      if (MOZ_UNLIKELY(wrapChildrenInAnotherBlockquoteResult.isErr())) {
        NS_WARNING(
            "HTMLEditor::WrapContentsInBlockquoteElementsWithTransaction() "
            "failed");
        return wrapChildrenInAnotherBlockquoteResult;
      }
      wrapChildrenInAnotherBlockquoteResult.MoveCaretPointTo(
          pointToPutCaret, {SuggestCaret::OnlyIfHasSuggestion});
      if (wrapChildrenInAnotherBlockquoteResult.GetNewNode()) {
        blockElementToPutCaret =
            wrapChildrenInAnotherBlockquoteResult.UnwrapNewNode();
      }
    }

    // If the node has different parent than previous node, further nodes in a
    // new parent
    if (prevParent) {
      if (prevParent != content->GetParentNode()) {
        // Forget any previous blockquote node we were using
        curBlock = nullptr;
        prevParent = content->GetParentNode();
      }
    } else {
      prevParent = content->GetParentNode();
    }

    // If no curBlock, make one
    if (!curBlock) {
      CreateElementResult createNewBlockQuoteElementResult =
          InsertElementWithSplittingAncestorsWithTransaction(
              *nsGkAtoms::blockquote, EditorDOMPoint(content),
              BRElementNextToSplitPoint::Keep, aEditingHost);
      if (createNewBlockQuoteElementResult.isErr()) {
        NS_WARNING(
            "HTMLEditor::InsertElementWithSplittingAncestorsWithTransaction("
            "nsGkAtoms::blockquote) failed");
        return createNewBlockQuoteElementResult;
      }
      createNewBlockQuoteElementResult.MoveCaretPointTo(
          pointToPutCaret, {SuggestCaret::OnlyIfHasSuggestion});
      MOZ_ASSERT(createNewBlockQuoteElementResult.GetNewNode());
      blockElementToPutCaret = createNewBlockQuoteElementResult.GetNewNode();
      curBlock = createNewBlockQuoteElementResult.UnwrapNewNode();
    }

    // MOZ_KnownLive because 'aArrayOfContents' is guaranteed to/ keep it alive.
    MoveNodeResult moveNodeResult =
        MoveNodeToEndWithTransaction(MOZ_KnownLive(content), *curBlock);
    if (moveNodeResult.isErr()) {
      NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
      return CreateElementResult(moveNodeResult.unwrapErr());
    }
    moveNodeResult.MoveCaretPointTo(pointToPutCaret,
                                    {SuggestCaret::OnlyIfHasSuggestion});
  }
  return blockElementToPutCaret
             ? CreateElementResult(std::move(blockElementToPutCaret),
                                   std::move(pointToPutCaret))
             : CreateElementResult::NotHandled(std::move(pointToPutCaret));
}

Result<EditorDOMPoint, nsresult>
HTMLEditor::RemoveBlockContainerElementsWithTransaction(
    const nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  // Intent of this routine is to be used for converting to/from headers,
  // paragraphs, pre, and address.  Those blocks that pretty much just contain
  // inline things...
  RefPtr<Element> blockElement;
  nsCOMPtr<nsIContent> firstContent, lastContent;
  EditorDOMPoint pointToPutCaret;
  for (auto& content : aArrayOfContents) {
    // If curNode is an <address>, <p>, <hn>, or <pre>, remove it.
    if (HTMLEditUtils::IsFormatNode(content)) {
      // Process any partial progress saved
      if (blockElement) {
        SplitRangeOffFromNodeResult unwrapBlockElementResult =
            RemoveBlockContainerElementWithTransactionBetween(
                *blockElement, *firstContent, *lastContent);
        if (unwrapBlockElementResult.isErr()) {
          NS_WARNING(
              "HTMLEditor::RemoveBlockContainerElementWithTransactionBetween() "
              "failed");
          return Err(unwrapBlockElementResult.unwrapErr());
        }
        unwrapBlockElementResult.MoveCaretPointTo(
            pointToPutCaret, {SuggestCaret::OnlyIfHasSuggestion});
        firstContent = lastContent = blockElement = nullptr;
      }
      if (!EditorUtils::IsEditableContent(content, EditorType::HTML)) {
        continue;
      }
      // Remove current block
      Result<EditorDOMPoint, nsresult> unwrapFormatBlockResult =
          RemoveBlockContainerWithTransaction(
              MOZ_KnownLive(*content->AsElement()));
      if (MOZ_UNLIKELY(unwrapFormatBlockResult.isErr())) {
        NS_WARNING("HTMLEditor::RemoveBlockContainerWithTransaction() failed");
        return unwrapFormatBlockResult;
      }
      if (unwrapFormatBlockResult.inspect().IsSet()) {
        pointToPutCaret = unwrapFormatBlockResult.unwrap();
      }
      continue;
    }

    // XXX How about, <th>, <thead>, <tfoot>, <dt>, <dl>?
    if (content->IsAnyOfHTMLElements(
            nsGkAtoms::table, nsGkAtoms::tr, nsGkAtoms::tbody, nsGkAtoms::td,
            nsGkAtoms::li, nsGkAtoms::blockquote, nsGkAtoms::div) ||
        HTMLEditUtils::IsAnyListElement(content)) {
      // Process any partial progress saved
      if (blockElement) {
        SplitRangeOffFromNodeResult unwrapBlockElementResult =
            RemoveBlockContainerElementWithTransactionBetween(
                *blockElement, *firstContent, *lastContent);
        if (unwrapBlockElementResult.isErr()) {
          NS_WARNING(
              "HTMLEditor::RemoveBlockContainerElementWithTransactionBetween() "
              "failed");
          return Err(unwrapBlockElementResult.unwrapErr());
        }
        unwrapBlockElementResult.MoveCaretPointTo(
            pointToPutCaret, {SuggestCaret::OnlyIfHasSuggestion});
        firstContent = lastContent = blockElement = nullptr;
      }
      if (!EditorUtils::IsEditableContent(content, EditorType::HTML)) {
        continue;
      }
      // Recursion time
      AutoTArray<OwningNonNull<nsIContent>, 24> childContents;
      HTMLEditor::GetChildNodesOf(*content, childContents);
      Result<EditorDOMPoint, nsresult> removeBlockContainerElementsResult =
          RemoveBlockContainerElementsWithTransaction(childContents);
      if (MOZ_UNLIKELY(removeBlockContainerElementsResult.isErr())) {
        NS_WARNING(
            "HTMLEditor::RemoveBlockContainerElementsWithTransaction() failed");
        return removeBlockContainerElementsResult;
      }
      if (removeBlockContainerElementsResult.inspect().IsSet()) {
        pointToPutCaret = removeBlockContainerElementsResult.unwrap();
      }
      continue;
    }

    if (HTMLEditUtils::IsInlineElement(content)) {
      if (blockElement) {
        // If so, is this node a descendant?
        if (EditorUtils::IsDescendantOf(*content, *blockElement)) {
          // Then we don't need to do anything different for this node
          lastContent = content;
          continue;
        }
        // Otherwise, we have progressed beyond end of blockElement, so let's
        // handle it now.  We need to remove the portion of blockElement that
        // contains [firstContent - lastContent].
        SplitRangeOffFromNodeResult unwrapBlockElementResult =
            RemoveBlockContainerElementWithTransactionBetween(
                *blockElement, *firstContent, *lastContent);
        if (unwrapBlockElementResult.isErr()) {
          NS_WARNING(
              "HTMLEditor::RemoveBlockContainerElementWithTransactionBetween() "
              "failed");
          return Err(unwrapBlockElementResult.unwrapErr());
        }
        unwrapBlockElementResult.MoveCaretPointTo(
            pointToPutCaret, {SuggestCaret::OnlyIfHasSuggestion});
        firstContent = lastContent = blockElement = nullptr;
        // Fall out and handle content
      }
      blockElement = HTMLEditUtils::GetAncestorElement(
          content, HTMLEditUtils::ClosestEditableBlockElement);
      if (!blockElement || !HTMLEditUtils::IsFormatNode(blockElement) ||
          !HTMLEditUtils::IsRemovableNode(*blockElement)) {
        // Not a block kind that we care about.
        blockElement = nullptr;
      } else {
        firstContent = lastContent = content;
      }
      continue;
    }

    if (blockElement) {
      // Some node that is already sans block style.  Skip over it and process
      // any partial progress saved.
      SplitRangeOffFromNodeResult unwrapBlockElementResult =
          RemoveBlockContainerElementWithTransactionBetween(
              *blockElement, *firstContent, *lastContent);
      if (unwrapBlockElementResult.isErr()) {
        NS_WARNING(
            "HTMLEditor::RemoveBlockContainerElementWithTransactionBetween() "
            "failed");
        return Err(unwrapBlockElementResult.unwrapErr());
      }
      unwrapBlockElementResult.MoveCaretPointTo(
          pointToPutCaret, {SuggestCaret::OnlyIfHasSuggestion});
      firstContent = lastContent = blockElement = nullptr;
      continue;
    }
  }
  // Process any partial progress saved
  if (blockElement) {
    SplitRangeOffFromNodeResult unwrapBlockElementResult =
        RemoveBlockContainerElementWithTransactionBetween(
            *blockElement, *firstContent, *lastContent);
    if (unwrapBlockElementResult.isErr()) {
      NS_WARNING(
          "HTMLEditor::RemoveBlockContainerElementWithTransactionBetween() "
          "failed");
      return Err(unwrapBlockElementResult.unwrapErr());
    }
    unwrapBlockElementResult.MoveCaretPointTo(
        pointToPutCaret, {SuggestCaret::OnlyIfHasSuggestion});
    firstContent = lastContent = blockElement = nullptr;
  }
  return pointToPutCaret;
}

CreateElementResult HTMLEditor::CreateOrChangeBlockContainerElement(
    nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents, nsAtom& aBlockTag,
    const Element& aEditingHost) {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  // Intent of this routine is to be used for converting to/from headers,
  // paragraphs, pre, and address.  Those blocks that pretty much just contain
  // inline things...
  RefPtr<Element> newBlock, curBlock, blockElementToPutCaret;
  EditorDOMPoint pointToPutCaret;
  for (auto& content : aArrayOfContents) {
    EditorDOMPoint atContent(content);
    if (NS_WARN_IF(!atContent.IsInContentNode())) {
      // If given node has been removed from the document, let's ignore it
      // since the following code may need its parent replace it with new
      // block.
      curBlock = nullptr;
      newBlock = nullptr;
      continue;
    }

    // Is it already the right kind of block, or an uneditable block?
    if (content->IsHTMLElement(&aBlockTag) ||
        (!EditorUtils::IsEditableContent(content, EditorType::HTML) &&
         HTMLEditUtils::IsBlockElement(content))) {
      // Forget any previous block used for previous inline nodes
      curBlock = nullptr;
      // Do nothing to this block
      continue;
    }

    // If content is a address, p, header, address, or pre, replace it with a
    // new block of correct type.
    // XXX: pre can't hold everything the others can
    if (HTMLEditUtils::IsMozDiv(content) ||
        HTMLEditUtils::IsFormatNode(content)) {
      // Forget any previous block used for previous inline nodes
      curBlock = nullptr;
      CreateElementResult newBlockElementOrError =
          ReplaceContainerAndCloneAttributesWithTransaction(
              MOZ_KnownLive(*content->AsElement()), aBlockTag);
      if (newBlockElementOrError.isErr()) {
        NS_WARNING(
            "EditorBase::ReplaceContainerAndCloneAttributesWithTransaction() "
            "failed");
        return newBlockElementOrError;
      }
      // If the new block element was moved to different element or removed by
      // the web app via mutation event listener, we should stop handling this
      // action since we cannot handle each of a lot of edge cases.
      if (NS_WARN_IF(newBlockElementOrError.GetNewNode()->GetParentNode() !=
                     atContent.GetContainer())) {
        return CreateElementResult(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
      }
      newBlockElementOrError.MoveCaretPointTo(
          pointToPutCaret, {SuggestCaret::OnlyIfHasSuggestion});
      newBlock = newBlockElementOrError.UnwrapNewNode();
      continue;
    }

    if (HTMLEditUtils::IsTable(content) ||
        HTMLEditUtils::IsAnyListElement(content) ||
        content->IsAnyOfHTMLElements(nsGkAtoms::tbody, nsGkAtoms::tr,
                                     nsGkAtoms::td, nsGkAtoms::li,
                                     nsGkAtoms::blockquote, nsGkAtoms::div)) {
      // Forget any previous block used for previous inline nodes
      curBlock = nullptr;
      // Recursion time
      AutoTArray<OwningNonNull<nsIContent>, 24> childContents;
      HTMLEditor::GetChildNodesOf(*content, childContents);
      if (!childContents.IsEmpty()) {
        CreateElementResult wrapChildrenInBlockElementResult =
            CreateOrChangeBlockContainerElement(childContents, aBlockTag,
                                                aEditingHost);
        if (MOZ_UNLIKELY(wrapChildrenInBlockElementResult.isErr())) {
          NS_WARNING(
              "HTMLEditor::CreateOrChangeBlockContainerElement() failed");
          return wrapChildrenInBlockElementResult;
        }
        wrapChildrenInBlockElementResult.MoveCaretPointTo(
            pointToPutCaret, {SuggestCaret::OnlyIfHasSuggestion});
        if (wrapChildrenInBlockElementResult.GetNewNode()) {
          blockElementToPutCaret =
              wrapChildrenInBlockElementResult.UnwrapNewNode();
        }
        continue;
      }

      // Make sure we can put a block here
      CreateElementResult createNewBlockElementResult =
          InsertElementWithSplittingAncestorsWithTransaction(
              aBlockTag, atContent, BRElementNextToSplitPoint::Keep,
              aEditingHost);
      if (createNewBlockElementResult.isErr()) {
        NS_WARNING(
            nsPrintfCString(
                "HTMLEditor::"
                "InsertElementWithSplittingAncestorsWithTransaction(%s) failed",
                nsAtomCString(&aBlockTag).get())
                .get());
        return createNewBlockElementResult;
      }
      createNewBlockElementResult.MoveCaretPointTo(
          pointToPutCaret, {SuggestCaret::OnlyIfHasSuggestion});
      MOZ_ASSERT(createNewBlockElementResult.GetNewNode());
      blockElementToPutCaret = createNewBlockElementResult.UnwrapNewNode();
      continue;
    }

    if (content->IsHTMLElement(nsGkAtoms::br)) {
      // If the node is a break, we honor it by putting further nodes in a new
      // parent
      if (curBlock) {
        // Forget any previous block used for previous inline nodes
        curBlock = nullptr;
        // MOZ_KnownLive because 'aArrayOfContents' is guaranteed to keep it
        // alive.
        nsresult rv = DeleteNodeWithTransaction(MOZ_KnownLive(*content));
        if (NS_FAILED(rv)) {
          NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
          return CreateElementResult(rv);
        }
        continue;
      }

      // The break is the first (or even only) node we encountered.  Create a
      // block for it.
      CreateElementResult createNewBlockElementResult =
          InsertElementWithSplittingAncestorsWithTransaction(
              aBlockTag, atContent, BRElementNextToSplitPoint::Keep,
              aEditingHost);
      if (createNewBlockElementResult.isErr()) {
        NS_WARNING(
            nsPrintfCString(
                "HTMLEditor::"
                "InsertElementWithSplittingAncestorsWithTransaction(%s) failed",
                nsAtomCString(&aBlockTag).get())
                .get());
        return createNewBlockElementResult;
      }
      createNewBlockElementResult.MoveCaretPointTo(
          pointToPutCaret, {SuggestCaret::OnlyIfHasSuggestion});
      RefPtr<Element> newBlockElement =
          createNewBlockElementResult.UnwrapNewNode();
      MOZ_ASSERT(newBlockElement);
      blockElementToPutCaret = newBlockElement;
      // MOZ_KnownLive because 'aArrayOfContents' is guaranteed to keep it
      // alive.
      MoveNodeResult moveNodeResult = MoveNodeToEndWithTransaction(
          MOZ_KnownLive(content), *newBlockElement);
      if (moveNodeResult.isErr()) {
        NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
        return CreateElementResult(moveNodeResult.unwrapErr());
      }
      moveNodeResult.MoveCaretPointTo(pointToPutCaret,
                                      {SuggestCaret::OnlyIfHasSuggestion});
      curBlock = std::move(newBlockElement);
      continue;
    }

    if (HTMLEditUtils::IsInlineElement(content)) {
      // If content is inline, pull it into curBlock.  Note: it's assumed that
      // consecutive inline nodes in aNodeArray are actually members of the
      // same block parent.  This happens to be true now as a side effect of
      // how aNodeArray is contructed, but some additional logic should be
      // added here if that should change
      //
      // If content is a non editable, drop it if we are going to <pre>.
      if (&aBlockTag == nsGkAtoms::pre &&
          !EditorUtils::IsEditableContent(content, EditorType::HTML)) {
        // Do nothing to this block
        continue;
      }

      // If no curBlock, make one
      if (!curBlock) {
        CreateElementResult createNewBlockElementResult =
            InsertElementWithSplittingAncestorsWithTransaction(
                aBlockTag, atContent, BRElementNextToSplitPoint::Keep,
                aEditingHost);
        if (createNewBlockElementResult.isErr()) {
          NS_WARNING(nsPrintfCString("HTMLEditor::"
                                     "InsertElementWithSplittingAncestorsWithTr"
                                     "ansaction(%s) failed",
                                     nsAtomCString(&aBlockTag).get())
                         .get());
          return createNewBlockElementResult;
        }
        createNewBlockElementResult.MoveCaretPointTo(
            pointToPutCaret, {SuggestCaret::OnlyIfHasSuggestion});
        MOZ_ASSERT(createNewBlockElementResult.GetNewNode());
        blockElementToPutCaret = createNewBlockElementResult.GetNewNode();
        curBlock = createNewBlockElementResult.UnwrapNewNode();

        // Update container of content.
        atContent.Set(content);
      }

      if (NS_WARN_IF(!atContent.IsSet())) {
        // This is possible due to mutation events, let's not assert
        return CreateElementResult(NS_ERROR_UNEXPECTED);
      }

      // XXX If content is a br, replace it with a return if going to <pre>

      // This is a continuation of some inline nodes that belong together in
      // the same block item.  Use curBlock.
      //
      // MOZ_KnownLive because 'aArrayOfContents' is guaranteed to keep it
      // alive.  We could try to make that a rvalue ref and create a const array
      // on the stack here, but callers are passing in auto arrays, and we don't
      // want to introduce copies..
      MoveNodeResult moveNodeResult =
          MoveNodeToEndWithTransaction(MOZ_KnownLive(content), *curBlock);
      if (moveNodeResult.isErr()) {
        NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
        return CreateElementResult(moveNodeResult.unwrapErr());
      }
      moveNodeResult.MoveCaretPointTo(pointToPutCaret,
                                      {SuggestCaret::OnlyIfHasSuggestion});
    }
  }
  return blockElementToPutCaret
             ? CreateElementResult(std::move(blockElementToPutCaret),
                                   std::move(pointToPutCaret))
             : CreateElementResult::NotHandled(std::move(pointToPutCaret));
}

SplitNodeResult HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction(
    nsAtom& aTag, const EditorDOMPoint& aStartOfDeepestRightNode,
    const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  if (NS_WARN_IF(!aStartOfDeepestRightNode.IsSet())) {
    return SplitNodeResult(NS_ERROR_INVALID_ARG);
  }
  MOZ_ASSERT(aStartOfDeepestRightNode.IsSetAndValid());

  // The point must be descendant of editing host.
  // XXX Isn't it a valid case if it points a direct child of aEditingHost?
  if (aStartOfDeepestRightNode.GetContainer() != &aEditingHost &&
      !EditorUtils::IsDescendantOf(*aStartOfDeepestRightNode.GetContainer(),
                                   aEditingHost)) {
    NS_WARNING("aStartOfDeepestRightNode was not in editing host");
    return SplitNodeResult(NS_ERROR_INVALID_ARG);
  }

  // Look for a node that can legally contain the tag.
  EditorDOMPoint pointToInsert(aStartOfDeepestRightNode);
  for (; pointToInsert.IsSet();
       pointToInsert.Set(pointToInsert.GetContainer())) {
    // We cannot split active editing host and its ancestor.  So, there is
    // no element to contain the specified element.
    if (pointToInsert.GetChild() == &aEditingHost) {
      NS_WARNING(
          "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction() reached "
          "editing host");
      return SplitNodeResult(NS_ERROR_FAILURE);
    }

    if (HTMLEditUtils::CanNodeContain(*pointToInsert.GetContainer(), aTag)) {
      // Found an ancestor node which can contain the element.
      break;
    }
  }

  MOZ_DIAGNOSTIC_ASSERT(pointToInsert.IsSet());

  // If the point itself can contain the tag, we don't need to split any
  // ancestor nodes.  In this case, we should return the given split point
  // as is.
  if (pointToInsert.GetContainer() == aStartOfDeepestRightNode.GetContainer()) {
    return SplitNodeResult::NotHandled(aStartOfDeepestRightNode,
                                       GetSplitNodeDirection());
  }

  SplitNodeResult splitNodeResult = SplitNodeDeepWithTransaction(
      MOZ_KnownLive(*pointToInsert.GetChild()), aStartOfDeepestRightNode,
      SplitAtEdges::eAllowToCreateEmptyContainer);
  NS_WARNING_ASSERTION(splitNodeResult.isOk(),
                       "HTMLEditor::SplitNodeDeepWithTransaction(SplitAtEdges::"
                       "eAllowToCreateEmptyContainer) failed");
  return splitNodeResult;
}

CreateElementResult
HTMLEditor::InsertElementWithSplittingAncestorsWithTransaction(
    nsAtom& aTagName, const EditorDOMPoint& aPointToInsert,
    BRElementNextToSplitPoint aBRElementNextToSplitPoint,
    const Element& aEditingHost,
    const InitializeInsertingElement& aInitializer) {
  MOZ_ASSERT(aPointToInsert.IsSetAndValid());

  const SplitNodeResult splitNodeResult =
      MaybeSplitAncestorsForInsertWithTransaction(aTagName, aPointToInsert,
                                                  aEditingHost);
  if (splitNodeResult.isErr()) {
    NS_WARNING(
        "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction() failed");
    return CreateElementResult(splitNodeResult.unwrapErr());
  }
  DebugOnly<bool> wasCaretPositionSuggestedAtSplit =
      splitNodeResult.HasCaretPointSuggestion();
  // We'll update selection below, and nobody touches selection until then.
  // Therefore, we don't need to touch selection here.
  splitNodeResult.IgnoreCaretPointSuggestion();

  // If current handling node has been moved from the container by a
  // mutation event listener when we need to do something more for it,
  // we should stop handling this action since we cannot handle each of
  // a lot of edge cases.
  if (NS_WARN_IF(aPointToInsert.HasChildMovedFromContainer())) {
    return CreateElementResult(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  EditorDOMPoint splitPoint = splitNodeResult.AtSplitPoint<EditorDOMPoint>();

  if (aBRElementNextToSplitPoint == BRElementNextToSplitPoint::Delete) {
    // Consume a trailing br, if any.  This is to keep an alignment from
    // creating extra lines, if possible.
    if (nsCOMPtr<nsIContent> maybeBRContent = HTMLEditUtils::GetNextContent(
            splitPoint,
            {WalkTreeOption::IgnoreNonEditableNode,
             WalkTreeOption::StopAtBlockBoundary},
            &aEditingHost)) {
      if (maybeBRContent->IsHTMLElement(nsGkAtoms::br) &&
          splitPoint.GetChild()) {
        // Making use of html structure... if next node after where we are
        // putting our div is not a block, then the br we found is in same
        // block we are, so it's safe to consume it.
        if (nsIContent* nextEditableSibling = HTMLEditUtils::GetNextSibling(
                *splitPoint.GetChild(),
                {WalkTreeOption::IgnoreNonEditableNode})) {
          if (!HTMLEditUtils::IsBlockElement(*nextEditableSibling)) {
            AutoEditorDOMPointChildInvalidator lockOffset(splitPoint);
            nsresult rv = DeleteNodeWithTransaction(*maybeBRContent);
            if (NS_FAILED(rv)) {
              NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
              return CreateElementResult(rv);
            }
          }
        }
      }
    }
  }

  CreateElementResult createNewElementResult = CreateAndInsertElement(
      WithTransaction::Yes, aTagName, splitPoint, aInitializer);
  if (createNewElementResult.isErr()) {
    NS_WARNING(
        "HTMLEditor::CreateAndInsertElement(WithTransaction::Yes) failed");
    return CreateElementResult(createNewElementResult.unwrapErr());
  }
  MOZ_ASSERT_IF(wasCaretPositionSuggestedAtSplit,
                createNewElementResult.HasCaretPointSuggestion());
  MOZ_ASSERT(createNewElementResult.GetNewNode());

  // If the new block element was moved to different element or removed by
  // the web app via mutation event listener, we should stop handling this
  // action since we cannot handle each of a lot of edge cases.
  if (NS_WARN_IF(createNewElementResult.GetNewNode()->GetParentNode() !=
                 splitPoint.GetContainer())) {
    return CreateElementResult(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  return createNewElementResult;
}

nsresult HTMLEditor::JoinNearestEditableNodesWithTransaction(
    nsIContent& aNodeLeft, nsIContent& aNodeRight,
    EditorDOMPoint* aNewFirstChildOfRightNode) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aNewFirstChildOfRightNode);

  // Caller responsible for left and right node being the same type
  if (NS_WARN_IF(!aNodeLeft.GetParentNode())) {
    return NS_ERROR_FAILURE;
  }
  // If they don't have the same parent, first move the right node to after
  // the left one
  if (aNodeLeft.GetParentNode() != aNodeRight.GetParentNode()) {
    const MoveNodeResult moveNodeResult =
        MoveNodeWithTransaction(aNodeRight, EditorDOMPoint(&aNodeLeft));
    if (moveNodeResult.isErr()) {
      NS_WARNING("HTMLEditor::MoveNodeWithTransaction() failed");
      return moveNodeResult.unwrapErr();
    }
    nsresult rv = moveNodeResult.SuggestCaretPointTo(
        *this, {SuggestCaret::OnlyIfHasSuggestion,
                SuggestCaret::OnlyIfTransactionsAllowedToDoIt,
                SuggestCaret::AndIgnoreTrivialError});
    if (NS_FAILED(rv)) {
      NS_WARNING("MoveNodeResult::SuggestCaretPointTo() failed");
      return rv;
    }
    NS_WARNING_ASSERTION(
        rv != NS_SUCCESS_EDITOR_BUT_IGNORED_TRIVIAL_ERROR,
        "MoveNodeResult::SuggestCaretPointTo() failed, but ignored");
  }

  // Separate join rules for differing blocks
  if (HTMLEditUtils::IsAnyListElement(&aNodeLeft) || aNodeLeft.IsText()) {
    // For lists, merge shallow (wouldn't want to combine list items)
    JoinNodesResult joinNodesResult =
        JoinNodesWithTransaction(aNodeLeft, aNodeRight);
    if (MOZ_UNLIKELY(joinNodesResult.Failed())) {
      NS_WARNING("HTMLEditor::JoinNodesWithTransaction failed");
      return joinNodesResult.Rv();
    }
    *aNewFirstChildOfRightNode =
        joinNodesResult.AtJoinedPoint<EditorDOMPoint>();
    return joinNodesResult.Rv();
  }

  // Remember the last left child, and first right child
  nsCOMPtr<nsIContent> lastEditableChildOfLeftContent =
      HTMLEditUtils::GetLastChild(aNodeLeft,
                                  {WalkTreeOption::IgnoreNonEditableNode});
  if (MOZ_UNLIKELY(NS_WARN_IF(!lastEditableChildOfLeftContent))) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIContent> firstEditableChildOfRightContent =
      HTMLEditUtils::GetFirstChild(aNodeRight,
                                   {WalkTreeOption::IgnoreNonEditableNode});
  if (NS_WARN_IF(!firstEditableChildOfRightContent)) {
    return NS_ERROR_FAILURE;
  }

  // For list items, divs, etc., merge smart
  JoinNodesResult joinNodesResult =
      JoinNodesWithTransaction(aNodeLeft, aNodeRight);
  if (MOZ_UNLIKELY(joinNodesResult.Failed())) {
    NS_WARNING("HTMLEditor::JoinNodesWithTransaction() failed");
    return joinNodesResult.Rv();
  }

  if ((lastEditableChildOfLeftContent->IsText() ||
       lastEditableChildOfLeftContent->IsElement()) &&
      HTMLEditUtils::CanContentsBeJoined(*lastEditableChildOfLeftContent,
                                         *firstEditableChildOfRightContent,
                                         StyleDifference::CompareIfElements)) {
    nsresult rv = JoinNearestEditableNodesWithTransaction(
        *lastEditableChildOfLeftContent, *firstEditableChildOfRightContent,
        aNewFirstChildOfRightNode);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::JoinNearestEditableNodesWithTransaction() failed");
    return rv;
  }
  *aNewFirstChildOfRightNode = joinNodesResult.AtJoinedPoint<EditorDOMPoint>();
  return NS_OK;
}

Element* HTMLEditor::GetMostDistantAncestorMailCiteElement(
    const nsINode& aNode) const {
  Element* mailCiteElement = nullptr;
  const bool isPlaintextEditor = IsInPlaintextMode();
  for (Element* element : aNode.InclusiveAncestorsOfType<Element>()) {
    if ((isPlaintextEditor && element->IsHTMLElement(nsGkAtoms::pre)) ||
        HTMLEditUtils::IsMailCite(*element)) {
      mailCiteElement = element;
      continue;
    }
    if (element->IsHTMLElement(nsGkAtoms::body)) {
      break;
    }
  }
  return mailCiteElement;
}

nsresult HTMLEditor::CacheInlineStyles(nsIContent& aContent) {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  nsresult rv = GetInlineStyles(
      aContent, *TopLevelEditSubActionDataRef().mCachedInlineStyles);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::GetInlineStyles() failed");
  return rv;
}

nsresult HTMLEditor::GetInlineStyles(nsIContent& aContent,
                                     AutoStyleCacheArray& aStyleCacheArray) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aStyleCacheArray.IsEmpty());

  bool useCSS = IsCSSEnabled();

  for (nsStaticAtom* property : {nsGkAtoms::b,
                                 nsGkAtoms::i,
                                 nsGkAtoms::u,
                                 nsGkAtoms::s,
                                 nsGkAtoms::strike,
                                 nsGkAtoms::face,
                                 nsGkAtoms::size,
                                 nsGkAtoms::color,
                                 nsGkAtoms::tt,
                                 nsGkAtoms::em,
                                 nsGkAtoms::strong,
                                 nsGkAtoms::dfn,
                                 nsGkAtoms::code,
                                 nsGkAtoms::samp,
                                 nsGkAtoms::var,
                                 nsGkAtoms::cite,
                                 nsGkAtoms::abbr,
                                 nsGkAtoms::acronym,
                                 nsGkAtoms::backgroundColor,
                                 nsGkAtoms::sub,
                                 nsGkAtoms::sup}) {
    nsStaticAtom *tag, *attribute;
    if (property == nsGkAtoms::face || property == nsGkAtoms::size ||
        property == nsGkAtoms::color) {
      tag = nsGkAtoms::font;
      attribute = property;
    } else {
      tag = property;
      attribute = nullptr;
    }
    // If type-in state is set, don't intervene
    bool typeInSet, unused;
    mTypeInState->GetTypingState(typeInSet, unused, tag, attribute, nullptr);
    if (typeInSet) {
      continue;
    }
    bool isSet = false;
    nsString value;  // Don't use nsAutoString here because it requires memcpy
                     // at creating new StyleCache instance.
    // Don't use CSS for <font size>, we don't support it usefully (bug 780035)
    if (!useCSS || (property == nsGkAtoms::size)) {
      isSet = HTMLEditUtils::IsInlineStyleSetByElement(
          aContent, *tag, attribute, nullptr, &value);
    } else {
      Result<bool, nsresult> isComputedCSSEquivalentToHTMLInlineStyleOrError =
          mCSSEditUtils->IsComputedCSSEquivalentToHTMLInlineStyleSet(
              aContent, MOZ_KnownLive(tag), MOZ_KnownLive(attribute), value);
      if (isComputedCSSEquivalentToHTMLInlineStyleOrError.isErr()) {
        NS_WARNING(
            "CSSEditUtils::IsComputedCSSEquivalentToHTMLInlineStyleSet failed");
        return isComputedCSSEquivalentToHTMLInlineStyleOrError.unwrapErr();
      }
      isSet = isComputedCSSEquivalentToHTMLInlineStyleOrError.unwrap();
    }
    if (isSet) {
      aStyleCacheArray.AppendElement(StyleCache(tag, attribute, value));
    }
  }
  return NS_OK;
}

nsresult HTMLEditor::ReapplyCachedStyles() {
  MOZ_ASSERT(IsTopLevelEditSubActionDataAvailable());

  // The idea here is to examine our cached list of styles and see if any have
  // been removed.  If so, add typeinstate for them, so that they will be
  // reinserted when new content is added.

  if (TopLevelEditSubActionDataRef().mCachedInlineStyles->IsEmpty() ||
      !SelectionRef().RangeCount()) {
    return NS_OK;
  }

  // remember if we are in css mode
  bool useCSS = IsCSSEnabled();

  const RangeBoundary& atStartOfSelection =
      SelectionRef().GetRangeAt(0)->StartRef();
  nsCOMPtr<nsIContent> startContainerContent =
      atStartOfSelection.Container() &&
              atStartOfSelection.Container()->IsContent()
          ? atStartOfSelection.Container()->AsContent()
          : nullptr;
  if (NS_WARN_IF(!startContainerContent)) {
    return NS_OK;
  }

  AutoStyleCacheArray styleCacheArrayAtInsertionPoint;
  nsresult rv =
      GetInlineStyles(*startContainerContent, styleCacheArrayAtInsertionPoint);
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::GetInlineStyles() failed, but ignored");
    return NS_OK;
  }

  for (StyleCache& styleCacheBeforeEdit :
       *TopLevelEditSubActionDataRef().mCachedInlineStyles) {
    bool isFirst = false, isAny = false, isAll = false;
    nsAutoString currentValue;
    if (useCSS) {
      // check computed style first in css case
      Result<bool, nsresult> isComputedCSSEquivalentToHTMLInlineStyleOrError =
          mCSSEditUtils->IsComputedCSSEquivalentToHTMLInlineStyleSet(
              *startContainerContent, MOZ_KnownLive(styleCacheBeforeEdit.Tag()),
              MOZ_KnownLive(styleCacheBeforeEdit.GetAttribute()), currentValue);
      if (isComputedCSSEquivalentToHTMLInlineStyleOrError.isErr()) {
        NS_WARNING(
            "CSSEditUtils::IsComputedCSSEquivalentToHTMLInlineStyleSet() "
            "failed");
        return isComputedCSSEquivalentToHTMLInlineStyleOrError.unwrapErr();
      }
      isAny = isComputedCSSEquivalentToHTMLInlineStyleOrError.unwrap();
    }
    if (!isAny) {
      // then check typeinstate and html style
      nsresult rv = GetInlinePropertyBase(
          MOZ_KnownLive(*styleCacheBeforeEdit.Tag()),
          MOZ_KnownLive(styleCacheBeforeEdit.GetAttribute()),
          &styleCacheBeforeEdit.Value(), &isFirst, &isAny, &isAll,
          &currentValue);
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::GetInlinePropertyBase() failed");
        return rv;
      }
    }
    // This style has disappeared through deletion.  Let's add the styles to
    // mTypeInState when same style isn't applied to the node already.
    if (isAny && !IsStyleCachePreservingSubAction(GetTopLevelEditSubAction())) {
      continue;
    }
    AutoStyleCacheArray::index_type index =
        styleCacheArrayAtInsertionPoint.IndexOf(
            styleCacheBeforeEdit.Tag(), styleCacheBeforeEdit.GetAttribute());
    if (index == AutoStyleCacheArray::NoIndex ||
        styleCacheBeforeEdit.Value() !=
            styleCacheArrayAtInsertionPoint.ElementAt(index).Value()) {
      mTypeInState->SetProp(styleCacheBeforeEdit.Tag(),
                            styleCacheBeforeEdit.GetAttribute(),
                            styleCacheBeforeEdit.Value());
    }
  }
  return NS_OK;
}

nsresult HTMLEditor::InsertBRElementToEmptyListItemsAndTableCellsInRange(
    const RawRangeBoundary& aStartRef, const RawRangeBoundary& aEndRef) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  AutoTArray<OwningNonNull<Element>, 64> arrayOfEmptyElements;
  DOMIterator iter;
  if (NS_FAILED(iter.Init(aStartRef, aEndRef))) {
    NS_WARNING("DOMIterator::Init() failed");
    return NS_ERROR_FAILURE;
  }
  iter.AppendNodesToArray(
      +[](nsINode& aNode, void* aSelf) {
        MOZ_ASSERT(Element::FromNode(&aNode));
        MOZ_ASSERT(aSelf);
        Element* element = aNode.AsElement();
        if (!EditorUtils::IsEditableContent(*element, EditorType::HTML) ||
            (!HTMLEditUtils::IsListItem(element) &&
             !HTMLEditUtils::IsTableCellOrCaption(*element))) {
          return false;
        }
        return HTMLEditUtils::IsEmptyNode(
            *element, {EmptyCheckOption::TreatSingleBRElementAsVisible});
      },
      arrayOfEmptyElements, this);

  // Put padding <br> elements for empty <li> and <td>.
  EditorDOMPoint pointToPutCaret;
  for (auto& emptyElement : arrayOfEmptyElements) {
    // Need to put br at END of node.  It may have empty containers in it and
    // still pass the "IsEmptyNode" test, and we want the br's to be after
    // them.  Also, we want the br to be after the selection if the selection
    // is in this node.
    EditorDOMPoint endOfNode(EditorDOMPoint::AtEndOf(emptyElement));
    CreateElementResult insertPaddingBRElementResult =
        InsertPaddingBRElementForEmptyLastLineWithTransaction(endOfNode);
    if (insertPaddingBRElementResult.isErr()) {
      NS_WARNING(
          "HTMLEditor::InsertPaddingBRElementForEmptyLastLineWithTransaction() "
          "failed");
      return insertPaddingBRElementResult.unwrapErr();
    }
    insertPaddingBRElementResult.MoveCaretPointTo(
        pointToPutCaret, *this,
        {SuggestCaret::OnlyIfHasSuggestion,
         SuggestCaret::OnlyIfTransactionsAllowedToDoIt});
  }
  if (pointToPutCaret.IsSet()) {
    nsresult rv = CollapseSelectionTo(pointToPutCaret);
    if (MOZ_UNLIKELY(rv == NS_ERROR_EDITOR_DESTROYED)) {
      NS_WARNING(
          "EditorBase::CollapseSelectionTo() caused destroying the editor");
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "EditorBase::CollapseSelectionTo() failed, but ignored");
  }
  return NS_OK;
}

void HTMLEditor::SetSelectionInterlinePosition() {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(SelectionRef().IsCollapsed());

  // Get the (collapsed) selection location
  const nsRange* firstRange = SelectionRef().GetRangeAt(0);
  if (NS_WARN_IF(!firstRange)) {
    return;
  }

  EditorDOMPoint atCaret(firstRange->StartRef());
  if (NS_WARN_IF(!atCaret.IsSet())) {
    return;
  }
  MOZ_ASSERT(atCaret.IsSetAndValid());

  // First, let's check to see if we are after a `<br>`.  We take care of this
  // special-case first so that we don't accidentally fall through into one of
  // the other conditionals.
  // XXX Although I don't understand "interline position", if caret is
  //     immediately after non-editable contents, but previous editable
  //     content is `<br>`, does this do right thing?
  if (Element* editingHost = ComputeEditingHost()) {
    if (nsIContent* previousEditableContentInBlock =
            HTMLEditUtils::GetPreviousContent(
                atCaret,
                {WalkTreeOption::IgnoreNonEditableNode,
                 WalkTreeOption::StopAtBlockBoundary},
                editingHost)) {
      if (previousEditableContentInBlock->IsHTMLElement(nsGkAtoms::br)) {
        DebugOnly<nsresult> rvIgnored = SelectionRef().SetInterlinePosition(
            InterlinePosition::StartOfNextLine);
        NS_WARNING_ASSERTION(
            NS_SUCCEEDED(rvIgnored),
            "Selection::SetInterlinePosition(InterlinePosition::"
            "StartOfNextLine) failed, but ignored");
        return;
      }
    }
  }

  if (!atCaret.GetChild()) {
    return;
  }

  // If caret is immediately after a block, set interline position to "right".
  // XXX Although I don't understand "interline position", if caret is
  //     immediately after non-editable contents, but previous editable
  //     content is a block, does this do right thing?
  if (nsIContent* previousEditableContentInBlockAtCaret =
          HTMLEditUtils::GetPreviousSibling(
              *atCaret.GetChild(), {WalkTreeOption::IgnoreNonEditableNode})) {
    if (HTMLEditUtils::IsBlockElement(*previousEditableContentInBlockAtCaret)) {
      DebugOnly<nsresult> rvIgnored = SelectionRef().SetInterlinePosition(
          InterlinePosition::StartOfNextLine);
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                           "Selection::SetInterlinePosition(InterlinePosition::"
                           "StartOfNextLine) failed, but ignored");
      return;
    }
  }

  // If caret is immediately before a block, set interline position to "left".
  // XXX Although I don't understand "interline position", if caret is
  //     immediately before non-editable contents, but next editable
  //     content is a block, does this do right thing?
  if (nsIContent* nextEditableContentInBlockAtCaret =
          HTMLEditUtils::GetNextSibling(
              *atCaret.GetChild(), {WalkTreeOption::IgnoreNonEditableNode})) {
    if (HTMLEditUtils::IsBlockElement(*nextEditableContentInBlockAtCaret)) {
      DebugOnly<nsresult> rvIgnored =
          SelectionRef().SetInterlinePosition(InterlinePosition::EndOfLine);
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                           "Selection::SetInterlinePosition(InterlinePosition::"
                           "EndOfLine) failed, but ignored");
    }
  }
}

nsresult HTMLEditor::AdjustCaretPositionAndEnsurePaddingBRElement(
    nsIEditor::EDirection aDirectionAndAmount) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(SelectionRef().IsCollapsed());

  auto point = GetFirstSelectionStartPoint<EditorDOMPoint>();
  if (NS_WARN_IF(!point.IsInContentNode())) {
    return NS_ERROR_FAILURE;
  }

  // If selection start is not editable, climb up the tree until editable one.
  while (!EditorUtils::IsEditableContent(*point.ContainerAs<nsIContent>(),
                                         EditorType::HTML)) {
    point.Set(point.GetContainer());
    if (NS_WARN_IF(!point.IsInContentNode())) {
      return NS_ERROR_FAILURE;
    }
  }

  // If caret is in empty block element, we need to insert a `<br>` element
  // because the block should have one-line height.
  // XXX Even if only a part of the block is editable, shouldn't we put
  //     caret if the block element is now empty?
  if (Element* const editableBlockElement =
          HTMLEditUtils::GetInclusiveAncestorElement(
              *point.ContainerAs<nsIContent>(),
              HTMLEditUtils::ClosestEditableBlockElement)) {
    if (editableBlockElement &&
        HTMLEditUtils::IsEmptyNode(
            *editableBlockElement,
            {EmptyCheckOption::TreatSingleBRElementAsVisible}) &&
        HTMLEditUtils::CanNodeContain(*point.GetContainer(), *nsGkAtoms::br)) {
      Element* bodyOrDocumentElement = GetRoot();
      if (NS_WARN_IF(!bodyOrDocumentElement)) {
        return NS_ERROR_FAILURE;
      }
      if (point.GetContainer() == bodyOrDocumentElement) {
        // Our root node is completely empty. Don't add a <br> here.
        // AfterEditInner() will add one for us when it calls
        // EditorBase::MaybeCreatePaddingBRElementForEmptyEditor().
        // XXX This kind of dependency between methods makes us spaghetti.
        //     Let's handle it here later.
        // XXX This looks odd check.  If active editing host is not a
        //     `<body>`, what are we doing?
        return NS_OK;
      }
      CreateElementResult insertPaddingBRElementResult =
          InsertPaddingBRElementForEmptyLastLineWithTransaction(point);
      if (insertPaddingBRElementResult.isErr()) {
        NS_WARNING(
            "HTMLEditor::InsertPaddingBRElementForEmptyLastLineWithTransaction("
            ") failed");
        return insertPaddingBRElementResult.unwrapErr();
      }
      nsresult rv = insertPaddingBRElementResult.SuggestCaretPointTo(
          *this, {SuggestCaret::OnlyIfHasSuggestion,
                  SuggestCaret::OnlyIfTransactionsAllowedToDoIt,
                  SuggestCaret::AndIgnoreTrivialError});
      if (NS_FAILED(rv)) {
        NS_WARNING("CreateElementResult::SuggestCaretPointTo() failed");
        return rv;
      }
      NS_WARNING_ASSERTION(
          rv != NS_SUCCESS_EDITOR_BUT_IGNORED_TRIVIAL_ERROR,
          "CreateElementResult::SuggestCaretPointTo() failed, but ignored");
      return NS_OK;
    }
  }

  // XXX Perhaps, we should do something if we're in a data node but not
  //     a text node.
  if (point.IsInTextNode()) {
    return NS_OK;
  }

  // Do we need to insert a padding <br> element for empty last line?  We do
  // if we are:
  // 1) prior node is in same block where selection is AND
  // 2) prior node is a br AND
  // 3) that br is not visible
  RefPtr<Element> editingHost = ComputeEditingHost();
  if (!editingHost) {
    return NS_OK;
  }

  if (nsCOMPtr<nsIContent> previousEditableContent =
          HTMLEditUtils::GetPreviousContent(
              point, {WalkTreeOption::IgnoreNonEditableNode}, editingHost)) {
    // If caret and previous editable content are in same block element
    // (even if it's a non-editable element), we should put a padding <br>
    // element at end of the block.
    const Element* const blockElementContainingCaret =
        HTMLEditUtils::GetInclusiveAncestorElement(
            *point.ContainerAs<nsIContent>(),
            HTMLEditUtils::ClosestBlockElement);
    const Element* const blockElementContainingPreviousEditableContent =
        HTMLEditUtils::GetAncestorElement(*previousEditableContent,
                                          HTMLEditUtils::ClosestBlockElement);
    // If previous editable content of caret is in same block and a `<br>`
    // element, we need to adjust interline position.
    if (blockElementContainingCaret &&
        blockElementContainingCaret ==
            blockElementContainingPreviousEditableContent &&
        point.ContainerAs<nsIContent>()->GetEditingHost() ==
            previousEditableContent->GetEditingHost() &&
        previousEditableContent &&
        previousEditableContent->IsHTMLElement(nsGkAtoms::br)) {
      // If it's an invisible `<br>` element, we need to insert a padding
      // `<br>` element for making empty line have one-line height.
      if (HTMLEditUtils::IsInvisibleBRElement(*previousEditableContent) &&
          !EditorUtils::IsPaddingBRElementForEmptyLastLine(
              *previousEditableContent)) {
        CreateElementResult insertPaddingBRElementResult =
            InsertPaddingBRElementForEmptyLastLineWithTransaction(point);
        if (insertPaddingBRElementResult.isErr()) {
          NS_WARNING(
              "HTMLEditor::"
              "InsertPaddingBRElementForEmptyLastLineWithTransaction() failed");
          return insertPaddingBRElementResult.unwrapErr();
        }
        insertPaddingBRElementResult.IgnoreCaretPointSuggestion();
        nsresult rv = CollapseSelectionTo(
            EditorRawDOMPoint(insertPaddingBRElementResult.GetNewNode(),
                              InterlinePosition::StartOfNextLine));
        if (NS_FAILED(rv)) {
          NS_WARNING("EditorBase::CollapseSelectionTo() failed");
          return rv;
        }
      }
      // If it's a visible `<br>` element and next editable content is a
      // padding `<br>` element, we need to set interline position.
      else if (nsIContent* nextEditableContentInBlock =
                   HTMLEditUtils::GetNextContent(
                       *previousEditableContent,
                       {WalkTreeOption::IgnoreNonEditableNode,
                        WalkTreeOption::StopAtBlockBoundary},
                       editingHost)) {
        if (EditorUtils::IsPaddingBRElementForEmptyLastLine(
                *nextEditableContentInBlock)) {
          // Make it stick to the padding `<br>` element so that it will be
          // on blank line.
          DebugOnly<nsresult> rvIgnored = SelectionRef().SetInterlinePosition(
              InterlinePosition::StartOfNextLine);
          NS_WARNING_ASSERTION(
              NS_SUCCEEDED(rvIgnored),
              "Selection::SetInterlinePosition(InterlinePosition::"
              "StartOfNextLine) failed, but ignored");
        }
      }
    }
  }

  // If previous editable content in same block is `<br>`, text node, `<img>`
  //  or `<hr>`, current caret position is fine.
  if (nsIContent* previousEditableContentInBlock =
          HTMLEditUtils::GetPreviousContent(
              point,
              {WalkTreeOption::IgnoreNonEditableNode,
               WalkTreeOption::StopAtBlockBoundary},
              editingHost)) {
    if (previousEditableContentInBlock->IsHTMLElement(nsGkAtoms::br) ||
        previousEditableContentInBlock->IsText() ||
        HTMLEditUtils::IsImage(previousEditableContentInBlock) ||
        previousEditableContentInBlock->IsHTMLElement(nsGkAtoms::hr)) {
      return NS_OK;
    }
  }

  // If next editable content in same block is `<br>`, text node, `<img>` or
  // `<hr>`, current caret position is fine.
  if (nsIContent* nextEditableContentInBlock =
          HTMLEditUtils::GetNextContent(point,
                                        {WalkTreeOption::IgnoreNonEditableNode,
                                         WalkTreeOption::StopAtBlockBoundary},
                                        editingHost)) {
    if (nextEditableContentInBlock->IsText() ||
        nextEditableContentInBlock->IsAnyOfHTMLElements(
            nsGkAtoms::br, nsGkAtoms::img, nsGkAtoms::hr)) {
      return NS_OK;
    }
  }

  // Otherwise, look for a near editable content towards edit action direction.

  // If there is no editable content, keep current caret position.
  // XXX Why do we treat `nsIEditor::ePreviousWord` etc as forward direction?
  nsIContent* nearEditableContent = HTMLEditUtils::GetAdjacentContentToPutCaret(
      point,
      aDirectionAndAmount == nsIEditor::ePrevious ? WalkTreeDirection::Backward
                                                  : WalkTreeDirection::Forward,
      *editingHost);
  if (!nearEditableContent) {
    return NS_OK;
  }

  EditorRawDOMPoint pointToPutCaret =
      HTMLEditUtils::GetGoodCaretPointFor<EditorRawDOMPoint>(
          *nearEditableContent, aDirectionAndAmount);
  if (!pointToPutCaret.IsSet()) {
    NS_WARNING("HTMLEditUtils::GetGoodCaretPointFor() failed");
    return NS_ERROR_FAILURE;
  }
  nsresult rv = CollapseSelectionTo(pointToPutCaret);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::CollapseSelectionTo() failed");
  return rv;
}

nsresult HTMLEditor::RemoveEmptyNodesIn(const EditorDOMRange& aRange) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aRange.IsPositioned());

  // Some general notes on the algorithm used here: the goal is to examine all
  // the nodes in aRange, and remove the empty ones.  We do this by
  // using a content iterator to traverse all the nodes in the range, and
  // placing the empty nodes into an array.  After finishing the iteration,
  // we delete the empty nodes in the array.  (They cannot be deleted as we
  // find them because that would invalidate the iterator.)
  //
  // Since checking to see if a node is empty can be costly for nodes with
  // many descendants, there are some optimizations made.  I rely on the fact
  // that the iterator is post-order: it will visit children of a node before
  // visiting the parent node.  So if I find that a child node is not empty, I
  // know that its parent is not empty without even checking.  So I put the
  // parent on a "skipList" which is just a voidArray of nodes I can skip the
  // empty check on.  If I encounter a node on the skiplist, i skip the
  // processing for that node and replace its slot in the skiplist with that
  // node's parent.
  //
  // An interesting idea is to go ahead and regard parent nodes that are NOT
  // on the skiplist as being empty (without even doing the IsEmptyNode check)
  // on the theory that if they weren't empty, we would have encountered a
  // non-empty child earlier and thus put this parent node on the skiplist.
  //
  // Unfortunately I can't use that strategy here, because the range may
  // include some children of a node while excluding others.  Thus I could
  // find all the _examined_ children empty, but still not have an empty
  // parent.

  const RawRangeBoundary endOfRange = [&]() {
    // If the range is not collapsed and end of the range is start of a
    // container, it means that the inclusive ancestor empty element may be
    // created by splitting the left nodes.
    if (aRange.Collapsed() || !aRange.IsInContentNodes() ||
        !aRange.EndRef().IsStartOfContainer()) {
      return aRange.EndRef().ToRawRangeBoundary();
    }
    nsINode* const commonAncestor =
        nsContentUtils::GetClosestCommonInclusiveAncestor(
            aRange.StartRef().ContainerAs<nsIContent>(),
            aRange.EndRef().ContainerAs<nsIContent>());
    if (!commonAncestor) {
      return aRange.EndRef().ToRawRangeBoundary();
    }
    nsIContent* maybeRightContent = nullptr;
    for (nsIContent* content : aRange.EndRef()
                                   .ContainerAs<nsIContent>()
                                   ->InclusiveAncestorsOfType<nsIContent>()) {
      if (!HTMLEditUtils::IsSimplyEditableNode(*content) ||
          content == commonAncestor) {
        break;
      }
      if (aRange.StartRef().ContainerAs<nsIContent>() == content) {
        break;
      }
      EmptyCheckOptions options = {EmptyCheckOption::TreatListItemAsVisible,
                                   EmptyCheckOption::TreatTableCellAsVisible};
      if (!HTMLEditUtils::IsBlockElement(*content)) {
        options += EmptyCheckOption::TreatSingleBRElementAsVisible;
      }
      if (!HTMLEditUtils::IsEmptyNode(*content, options)) {
        break;
      }
      maybeRightContent = content;
    }
    if (!maybeRightContent) {
      return aRange.EndRef().ToRawRangeBoundary();
    }
    return EditorRawDOMPoint::After(*maybeRightContent).ToRawRangeBoundary();
  }();

  PostContentIterator postOrderIter;
  nsresult rv =
      postOrderIter.Init(aRange.StartRef().ToRawRangeBoundary(), endOfRange);
  if (NS_FAILED(rv)) {
    NS_WARNING("PostContentIterator::Init() failed");
    return rv;
  }

  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfEmptyContents,
      arrayOfEmptyCites;

  // Collect empty nodes first.
  {
    const bool isMailEditor = IsMailEditor();
    AutoTArray<OwningNonNull<nsIContent>, 64> knownNonEmptyContents;
    Maybe<AutoRangeArray> maybeSelectionRanges;
    for (; !postOrderIter.IsDone(); postOrderIter.Next()) {
      MOZ_ASSERT(postOrderIter.GetCurrentNode()->IsContent());

      nsIContent* content = postOrderIter.GetCurrentNode()->AsContent();
      nsIContent* parentContent = content->GetParent();

      size_t idx = knownNonEmptyContents.IndexOf(content);
      if (idx != decltype(knownNonEmptyContents)::NoIndex) {
        // This node is on our skip list.  Skip processing for this node, and
        // replace its value in the skip list with the value of its parent
        if (parentContent) {
          knownNonEmptyContents[idx] = parentContent;
        }
        continue;
      }

      const bool isEmptyNode = [&]() {
        if (!content->IsElement()) {
          return false;
        }
        const bool isMailCite =
            isMailEditor && HTMLEditUtils::IsMailCite(*content->AsElement());
        const bool isCandidate = [&]() {
          if (content->IsHTMLElement(nsGkAtoms::body)) {
            // Don't delete the body
            return false;
          }
          if (isMailCite || content->IsHTMLElement(nsGkAtoms::a) ||
              HTMLEditUtils::IsInlineStyle(content) ||
              HTMLEditUtils::IsAnyListElement(content) ||
              content->IsHTMLElement(nsGkAtoms::div)) {
            // Only consider certain nodes to be empty for purposes of removal
            return true;
          }
          if (HTMLEditUtils::IsFormatNode(content) ||
              HTMLEditUtils::IsListItem(content) ||
              content->IsHTMLElement(nsGkAtoms::blockquote)) {
            // These node types are candidates if selection is not in them.  If
            // it is one of these, don't delete if selection inside.  This is so
            // we can create empty headings, etc., for the user to type into.
            if (maybeSelectionRanges.isNothing()) {
              maybeSelectionRanges.emplace(SelectionRef());
            }
            return !maybeSelectionRanges
                        ->IsAtLeastOneContainerOfRangeBoundariesInclusiveDescendantOf(
                            *content);
          }
          return false;
        }();

        if (!isCandidate) {
          return false;
        }

        // We delete mailcites even if they have a solo br in them.  Other
        // nodes we require to be empty.
        HTMLEditUtils::EmptyCheckOptions options{
            EmptyCheckOption::TreatListItemAsVisible,
            EmptyCheckOption::TreatTableCellAsVisible};
        if (!isMailCite) {
          options += EmptyCheckOption::TreatSingleBRElementAsVisible;
        }
        if (!HTMLEditUtils::IsEmptyNode(*content, options)) {
          return false;
        }

        if (isMailCite) {
          // mailcites go on a separate list from other empty nodes
          arrayOfEmptyCites.AppendElement(*content);
        }
        // Don't delete non-editable nodes in this method because this is a
        // clean up method to remove unnecessary nodes of the result of
        // editing.  So, we shouldn't delete non-editable nodes which were
        // there before editing.  Additionally, if the element is some special
        // elements such as <body>, we shouldn't delete it.
        else if (HTMLEditUtils::IsSimplyEditableNode(*content) &&
                 HTMLEditUtils::IsRemovableNode(*content)) {
          arrayOfEmptyContents.AppendElement(*content);
        }
        return true;
      }();
      if (!isEmptyNode && parentContent) {
        knownNonEmptyContents.AppendElement(*parentContent);
      }
    }  // end of the for-loop iterating with postOrderIter
  }

  // now delete the empty nodes
  for (OwningNonNull<nsIContent>& emptyContent : arrayOfEmptyContents) {
    // MOZ_KnownLive due to bug 1622253
    nsresult rv = DeleteNodeWithTransaction(MOZ_KnownLive(emptyContent));
    if (NS_FAILED(rv)) {
      NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
      return rv;
    }
  }

  // Now delete the empty mailcites.  This is a separate step because we want
  // to pull out any br's and preserve them.
  EditorDOMPoint pointToPutCaret;
  for (OwningNonNull<nsIContent>& emptyCite : arrayOfEmptyCites) {
    if (!HTMLEditUtils::IsEmptyNode(
            emptyCite, {EmptyCheckOption::TreatSingleBRElementAsVisible,
                        EmptyCheckOption::TreatListItemAsVisible,
                        EmptyCheckOption::TreatTableCellAsVisible})) {
      // We are deleting a cite that has just a `<br>`.  We want to delete cite,
      // but preserve `<br>`.
      CreateElementResult insertBRElementResult =
          InsertBRElement(WithTransaction::Yes, EditorDOMPoint(emptyCite));
      if (insertBRElementResult.isErr()) {
        NS_WARNING("HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
        return insertBRElementResult.unwrapErr();
      }
      // XXX Is this intentional selection change?
      insertBRElementResult.MoveCaretPointTo(
          pointToPutCaret, *this,
          {SuggestCaret::OnlyIfHasSuggestion,
           SuggestCaret::OnlyIfTransactionsAllowedToDoIt});
      MOZ_ASSERT(insertBRElementResult.GetNewNode());
    }
    // MOZ_KnownLive because 'arrayOfEmptyCites' is guaranteed to keep it alive.
    nsresult rv = DeleteNodeWithTransaction(MOZ_KnownLive(emptyCite));
    if (NS_FAILED(rv)) {
      NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
      return rv;
    }
  }
  // XXX Is this intentional selection change?
  if (pointToPutCaret.IsSet()) {
    nsresult rv = CollapseSelectionTo(pointToPutCaret);
    if (MOZ_UNLIKELY(rv == NS_ERROR_EDITOR_DESTROYED)) {
      NS_WARNING(
          "EditorBase::CollapseSelectionTo() caused destroying the editor");
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "EditorBase::CollapseSelectionTo() failed, but ignored");
  }

  return NS_OK;
}

nsresult HTMLEditor::LiftUpListItemElement(
    Element& aListItemElement,
    LiftUpFromAllParentListElements aLiftUpFromAllParentListElements) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  if (!HTMLEditUtils::IsListItem(&aListItemElement)) {
    return NS_ERROR_INVALID_ARG;
  }

  if (NS_WARN_IF(!aListItemElement.GetParentElement()) ||
      NS_WARN_IF(!aListItemElement.GetParentElement()->GetParentNode())) {
    return NS_ERROR_FAILURE;
  }

  // if it's first or last list item, don't need to split the list
  // otherwise we do.
  bool isFirstListItem = HTMLEditUtils::IsFirstChild(
      aListItemElement, {WalkTreeOption::IgnoreNonEditableNode});
  bool isLastListItem = HTMLEditUtils::IsLastChild(
      aListItemElement, {WalkTreeOption::IgnoreNonEditableNode});

  Element* leftListElement = aListItemElement.GetParentElement();
  if (NS_WARN_IF(!leftListElement)) {
    return NS_ERROR_FAILURE;
  }

  // If it's at middle of parent list element, split the parent list element.
  // Then, aListItem becomes the first list item of the right list element.
  if (!isFirstListItem && !isLastListItem) {
    EditorDOMPoint atListItemElement(&aListItemElement);
    if (NS_WARN_IF(!atListItemElement.IsSet())) {
      return NS_ERROR_FAILURE;
    }
    MOZ_ASSERT(atListItemElement.IsSetAndValid());
    const SplitNodeResult splitListItemParentResult =
        SplitNodeWithTransaction(atListItemElement);
    if (splitListItemParentResult.isErr()) {
      NS_WARNING("HTMLEditor::SplitNodeWithTransaction() failed");
      return splitListItemParentResult.unwrapErr();
    }
    nsresult rv = splitListItemParentResult.SuggestCaretPointTo(
        *this, {SuggestCaret::OnlyIfTransactionsAllowedToDoIt});
    if (NS_FAILED(rv)) {
      NS_WARNING("SplitNodeResult::SuggestCaretPointTo() failed");
      return rv;
    }

    leftListElement =
        Element::FromNodeOrNull(splitListItemParentResult.GetPreviousContent());
    if (MOZ_UNLIKELY(!leftListElement)) {
      NS_WARNING(
          "HTMLEditor::SplitNodeWithTransaction() didn't return left list "
          "element");
      return NS_ERROR_FAILURE;
    }
  }

  // In most cases, insert the list item into the new left list node..
  EditorDOMPoint pointToInsertListItem(leftListElement);
  if (NS_WARN_IF(!pointToInsertListItem.IsSet())) {
    return NS_ERROR_FAILURE;
  }

  // But when the list item was the first child of the right list, it should
  // be inserted between the both list elements.  This allows user to hit
  // Enter twice at a list item breaks the parent list node.
  if (!isFirstListItem) {
    DebugOnly<bool> advanced = pointToInsertListItem.AdvanceOffset();
    NS_WARNING_ASSERTION(advanced,
                         "Failed to advance offset to right list node");
  }

  EditorDOMPoint pointToPutCaret;
  MoveNodeResult moveListItemElementResult =
      MoveNodeWithTransaction(aListItemElement, pointToInsertListItem);
  if (moveListItemElementResult.isErr()) {
    NS_WARNING("HTMLEditor::MoveNodeWithTransaction() failed");
    return moveListItemElementResult.unwrapErr();
  }
  moveListItemElementResult.MoveCaretPointTo(
      pointToPutCaret, *this,
      {SuggestCaret::OnlyIfHasSuggestion,
       SuggestCaret::OnlyIfTransactionsAllowedToDoIt});

  // Unwrap list item contents if they are no longer in a list
  // XXX If the parent list element is a child of another list element
  //     (although invalid tree), the list item element won't be unwrapped.
  //     That makes the parent ancestor element tree valid, but might be
  //     unexpected result.
  // XXX If aListItemElement is <dl> or <dd> and current parent is <ul> or <ol>,
  //     the list items won't be unwrapped.  If aListItemElement is <li> and its
  //     current parent is <dl>, there is same issue.
  if (!HTMLEditUtils::IsAnyListElement(pointToInsertListItem.GetContainer()) &&
      HTMLEditUtils::IsListItem(&aListItemElement)) {
    Result<EditorDOMPoint, nsresult> unwrapOrphanListItemElementResult =
        RemoveBlockContainerWithTransaction(aListItemElement);
    if (MOZ_UNLIKELY(unwrapOrphanListItemElementResult.isErr())) {
      NS_WARNING("HTMLEditor::RemoveBlockContainerWithTransaction() failed");
      return unwrapOrphanListItemElementResult.unwrapErr();
    }
    if (AllowsTransactionsToChangeSelection() &&
        unwrapOrphanListItemElementResult.inspect().IsSet()) {
      pointToPutCaret = unwrapOrphanListItemElementResult.unwrap();
    }
    if (!pointToPutCaret.IsSet()) {
      return NS_OK;
    }
    nsresult rv = CollapseSelectionTo(pointToPutCaret);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "EditorBase::CollapseSelectionTo() failed");
    return rv;
  }

  if (pointToPutCaret.IsSet()) {
    nsresult rv = CollapseSelectionTo(pointToPutCaret);
    if (MOZ_UNLIKELY(rv == NS_ERROR_EDITOR_DESTROYED)) {
      NS_WARNING("EditorBase::CollapseSelectionTo() failed");
      return rv;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "EditorBase::CollapseSelectionTo() failed, but ignored");
  }

  if (aLiftUpFromAllParentListElements == LiftUpFromAllParentListElements::No) {
    return NS_OK;
  }
  // XXX If aListItemElement is moved to unexpected element by mutation event
  //     listener, shouldn't we stop calling this?
  nsresult rv = LiftUpListItemElement(aListItemElement,
                                      LiftUpFromAllParentListElements::Yes);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "HTMLEditor::LiftUpListItemElement("
                       "LiftUpFromAllParentListElements::Yes) failed");
  return rv;
}

nsresult HTMLEditor::DestroyListStructureRecursively(Element& aListElement) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(HTMLEditUtils::IsAnyListElement(&aListElement));

  // XXX If mutation event listener inserts new child into `aListElement`,
  //     this becomes infinite loop so that we should set limit of the
  //     loop count from original child count.
  while (aListElement.GetFirstChild()) {
    OwningNonNull<nsIContent> child = *aListElement.GetFirstChild();

    if (HTMLEditUtils::IsListItem(child)) {
      // XXX Using LiftUpListItemElement() is too expensive for this purpose.
      //     Looks like the reason why this method uses it is, only this loop
      //     wants to work with first child of aListElement.  However, what it
      //     actually does is removing <li> as container.  Perhaps, we should
      //     decide destination first, and then, move contents in `child`.
      // XXX If aListElement is is a child of another list element (although
      //     it's invalid tree), this moves the list item to outside of
      //     aListElement's parent.  Is that really intentional behavior?
      nsresult rv = LiftUpListItemElement(
          MOZ_KnownLive(*child->AsElement()),
          HTMLEditor::LiftUpFromAllParentListElements::Yes);
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "HTMLEditor::LiftUpListItemElement(LiftUpFromAllParentListElements:"
            ":Yes) failed");
        return rv;
      }
      continue;
    }

    if (HTMLEditUtils::IsAnyListElement(child)) {
      nsresult rv =
          DestroyListStructureRecursively(MOZ_KnownLive(*child->AsElement()));
      if (NS_FAILED(rv)) {
        NS_WARNING("HTMLEditor::DestroyListStructureRecursively() failed");
        return rv;
      }
      continue;
    }

    // Delete any non-list items for now
    // XXX This is not HTML5 aware.  HTML5 allows all list elements to have
    //     <script> and <template> and <dl> element to have <div> to group
    //     some <dt> and <dd> elements.  So, this may break valid children.
    nsresult rv = DeleteNodeWithTransaction(*child);
    if (NS_FAILED(rv)) {
      NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
      return rv;
    }
  }

  // Delete the now-empty list
  const Result<EditorDOMPoint, nsresult> unwrapListElementResult =
      RemoveBlockContainerWithTransaction(aListElement);
  if (MOZ_UNLIKELY(unwrapListElementResult.isErr())) {
    NS_WARNING("HTMLEditor::RemoveBlockContainerWithTransaction() failed");
    return unwrapListElementResult.inspectErr();
  }
  const EditorDOMPoint& pointToPutCaret = unwrapListElementResult.inspect();
  if (!AllowsTransactionsToChangeSelection() || !pointToPutCaret.IsSet()) {
    return NS_OK;
  }
  nsresult rv = CollapseSelectionTo(pointToPutCaret);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::CollapseSelectionTo() failed");
  return rv;
}

nsresult HTMLEditor::EnsureSelectionInBodyOrDocumentElement() {
  MOZ_ASSERT(IsEditActionDataAvailable());

  RefPtr<Element> bodyOrDocumentElement = GetRoot();
  if (NS_WARN_IF(!bodyOrDocumentElement)) {
    return NS_ERROR_FAILURE;
  }

  const auto atCaret = GetFirstSelectionStartPoint<EditorRawDOMPoint>();
  if (NS_WARN_IF(!atCaret.IsSet())) {
    return NS_ERROR_FAILURE;
  }

  // XXX This does wrong things.  Web apps can put any elements as sibling
  //     of `<body>` element.  Therefore, this collapses `Selection` into
  //     the `<body>` element which `HTMLDocument.body` is set to.  So,
  //     this makes users impossible to modify content outside of the
  //     `<body>` element even if caret is in an editing host.

  // Check that selection start container is inside the <body> element.
  // XXXsmaug this code is insane.
  nsINode* temp = atCaret.GetContainer();
  while (temp && !temp->IsHTMLElement(nsGkAtoms::body)) {
    temp = temp->GetParentOrShadowHostNode();
  }

  // If we aren't in the <body> element, force the issue.
  if (!temp) {
    nsresult rv = CollapseSelectionToStartOf(*bodyOrDocumentElement);
    if (MOZ_UNLIKELY(rv == NS_ERROR_EDITOR_DESTROYED)) {
      NS_WARNING(
          "EditorBase::CollapseSelectionToStartOf() caused destroying the "
          "editor");
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "EditorBase::CollapseSelectionToStartOf() failed, but ignored");
    return NS_OK;
  }

  const auto selectionEndPoint = GetFirstSelectionEndPoint<EditorRawDOMPoint>();
  if (NS_WARN_IF(!selectionEndPoint.IsSet())) {
    return NS_ERROR_FAILURE;
  }

  // check that selNode is inside body
  // XXXsmaug this code is insane.
  temp = selectionEndPoint.GetContainer();
  while (temp && !temp->IsHTMLElement(nsGkAtoms::body)) {
    temp = temp->GetParentOrShadowHostNode();
  }

  // If we aren't in the <body> element, force the issue.
  if (!temp) {
    nsresult rv = CollapseSelectionToStartOf(*bodyOrDocumentElement);
    if (MOZ_UNLIKELY(rv == NS_ERROR_EDITOR_DESTROYED)) {
      NS_WARNING(
          "EditorBase::CollapseSelectionToStartOf() caused destroying the "
          "editor");
      return NS_ERROR_EDITOR_DESTROYED;
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "EditorBase::CollapseSelectionToStartOf() failed, but ignored");
  }

  return NS_OK;
}

nsresult HTMLEditor::InsertPaddingBRElementForEmptyLastLineIfNeeded(
    Element& aElement) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  if (!HTMLEditUtils::IsBlockElement(aElement)) {
    return NS_OK;
  }

  if (!HTMLEditUtils::IsEmptyNode(
          aElement, {EmptyCheckOption::TreatSingleBRElementAsVisible})) {
    return NS_OK;
  }

  CreateElementResult insertPaddingBRElementResult =
      InsertPaddingBRElementForEmptyLastLineWithTransaction(
          EditorDOMPoint(&aElement, 0u));
  if (insertPaddingBRElementResult.isErr()) {
    NS_WARNING(
        "HTMLEditor::InsertPaddingBRElementForEmptyLastLineWithTransaction() "
        "failed");
    return insertPaddingBRElementResult.unwrapErr();
  }
  nsresult rv = insertPaddingBRElementResult.SuggestCaretPointTo(
      *this, {SuggestCaret::OnlyIfHasSuggestion,
              SuggestCaret::OnlyIfTransactionsAllowedToDoIt,
              SuggestCaret::AndIgnoreTrivialError});
  if (NS_FAILED(rv)) {
    NS_WARNING("CreateElementResult::SuggestCaretPointTo() failed");
    return rv;
  }
  NS_WARNING_ASSERTION(
      rv != NS_SUCCESS_EDITOR_BUT_IGNORED_TRIVIAL_ERROR,
      "CreateElementResult::SuggestCaretPointTo() failed, but ignored");
  return NS_OK;
}

Result<EditorDOMPoint, nsresult> HTMLEditor::RemoveAlignFromDescendants(
    Element& aElement, const nsAString& aAlignType, EditTarget aEditTarget) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(!aElement.IsHTMLElement(nsGkAtoms::table));

  const bool useCSS = IsCSSEnabled();

  EditorDOMPoint pointToPutCaret;

  // Let's remove all alignment hints in the children of aNode; it can
  // be an ALIGN attribute (in case we just remove it) or a CENTER
  // element (here we have to remove the container and keep its
  // children). We break on tables and don't look at their children.
  nsCOMPtr<nsIContent> nextSibling;
  for (nsIContent* content =
           aEditTarget == EditTarget::NodeAndDescendantsExceptTable
               ? &aElement
               : aElement.GetFirstChild();
       content; content = nextSibling) {
    // Get the next sibling before removing content from the DOM tree.
    // XXX If next sibling is removed from the parent and/or inserted to
    //     different parent, we will behave unexpectedly.  I think that
    //     we should create child list and handle it with checking whether
    //     it's still a child of expected parent.
    nextSibling = aEditTarget == EditTarget::NodeAndDescendantsExceptTable
                      ? nullptr
                      : content->GetNextSibling();

    if (content->IsHTMLElement(nsGkAtoms::center)) {
      OwningNonNull<Element> centerElement = *content->AsElement();
      {
        Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
            RemoveAlignFromDescendants(centerElement, aAlignType,
                                       EditTarget::OnlyDescendantsExceptTable);
        if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
          NS_WARNING(
              "HTMLEditor::RemoveAlignFromDescendants(EditTarget::"
              "OnlyDescendantsExceptTable) failed");
          return pointToPutCaretOrError;
        }
        if (pointToPutCaretOrError.inspect().IsSet()) {
          pointToPutCaret = pointToPutCaretOrError.unwrap();
        }
      }

      // We may have to insert a `<br>` element before first child of the
      // `<center>` element because it should be first element of a hard line
      // even after removing the `<center>` element.
      {
        CreateElementResult maybeInsertBRElementBeforeFirstChildResult =
            EnsureHardLineBeginsWithFirstChildOf(centerElement);
        if (maybeInsertBRElementBeforeFirstChildResult.isErr()) {
          NS_WARNING(
              "HTMLEditor::EnsureHardLineBeginsWithFirstChildOf() failed");
          return Err(maybeInsertBRElementBeforeFirstChildResult.unwrapErr());
        }
        if (maybeInsertBRElementBeforeFirstChildResult
                .HasCaretPointSuggestion()) {
          pointToPutCaret =
              maybeInsertBRElementBeforeFirstChildResult.UnwrapCaretPoint();
        }
      }

      // We may have to insert a `<br>` element after last child of the
      // `<center>` element because it should be last element of a hard line
      // even after removing the `<center>` element.
      {
        CreateElementResult maybeInsertBRElementAfterLastChildResult =
            EnsureHardLineEndsWithLastChildOf(centerElement);
        if (maybeInsertBRElementAfterLastChildResult.isErr()) {
          NS_WARNING("HTMLEditor::EnsureHardLineEndsWithLastChildOf() failed");
          return Err(maybeInsertBRElementAfterLastChildResult.unwrapErr());
        }
        if (maybeInsertBRElementAfterLastChildResult
                .HasCaretPointSuggestion()) {
          pointToPutCaret =
              maybeInsertBRElementAfterLastChildResult.UnwrapCaretPoint();
        }
      }

      {
        Result<EditorDOMPoint, nsresult> unwrapCenterElementResult =
            RemoveContainerWithTransaction(centerElement);
        if (MOZ_UNLIKELY(unwrapCenterElementResult.isErr())) {
          NS_WARNING("HTMLEditor::RemoveContainerWithTransaction() failed");
          return Err(unwrapCenterElementResult.inspectErr());
        }
        if (unwrapCenterElementResult.inspect().IsSet()) {
          pointToPutCaret = unwrapCenterElementResult.unwrap();
        }
      }
      continue;
    }

    if (!HTMLEditUtils::IsBlockElement(*content) &&
        !content->IsHTMLElement(nsGkAtoms::hr)) {
      continue;
    }

    const OwningNonNull<Element> blockOrHRElement = *content->AsElement();
    if (HTMLEditUtils::SupportsAlignAttr(blockOrHRElement)) {
      nsresult rv =
          RemoveAttributeWithTransaction(blockOrHRElement, *nsGkAtoms::align);
      if (NS_WARN_IF(Destroyed())) {
        return Err(NS_ERROR_EDITOR_DESTROYED);
      }
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "EditorBase::RemoveAttributeWithTransaction(nsGkAtoms::align) "
            "failed");
        return Err(rv);
      }
    }
    if (useCSS) {
      if (blockOrHRElement->IsAnyOfHTMLElements(nsGkAtoms::table,
                                                nsGkAtoms::hr)) {
        nsresult rv = SetAttributeOrEquivalent(
            blockOrHRElement, nsGkAtoms::align, aAlignType, false);
        if (NS_WARN_IF(Destroyed())) {
          return Err(NS_ERROR_EDITOR_DESTROYED);
        }
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "EditorBase::SetAttributeOrEquivalent(nsGkAtoms::align) failed");
          return Err(rv);
        }
      } else {
        nsStyledElement* styledBlockOrHRElement =
            nsStyledElement::FromNode(blockOrHRElement);
        if (NS_WARN_IF(!styledBlockOrHRElement)) {
          return Err(NS_ERROR_FAILURE);
        }
        // MOZ_KnownLive(*styledBlockOrHRElement): It's `blockOrHRElement
        // which is OwningNonNull.
        nsAutoString dummyCssValue;
        Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
            mCSSEditUtils->RemoveCSSInlineStyleWithTransaction(
                MOZ_KnownLive(*styledBlockOrHRElement), nsGkAtoms::textAlign,
                dummyCssValue);
        if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
          NS_WARNING(
              "CSSEditUtils::RemoveCSSInlineStyleWithTransaction(nsGkAtoms::"
              "textAlign) failed");
          return pointToPutCaretOrError;
        }
        if (pointToPutCaretOrError.inspect().IsSet()) {
          pointToPutCaret = pointToPutCaretOrError.unwrap();
        }
      }
    }
    if (!blockOrHRElement->IsHTMLElement(nsGkAtoms::table)) {
      // unless this is a table, look at children
      Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
          RemoveAlignFromDescendants(blockOrHRElement, aAlignType,
                                     EditTarget::OnlyDescendantsExceptTable);
      if (pointToPutCaretOrError.isErr()) {
        NS_WARNING(
            "HTMLEditor::RemoveAlignFromDescendants(EditTarget::"
            "OnlyDescendantsExceptTable) failed");
        return pointToPutCaretOrError;
      }
      if (pointToPutCaretOrError.inspect().IsSet()) {
        pointToPutCaret = pointToPutCaretOrError.unwrap();
      }
    }
  }
  return pointToPutCaret;
}

CreateElementResult HTMLEditor::EnsureHardLineBeginsWithFirstChildOf(
    Element& aRemovingContainerElement) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  nsIContent* firstEditableChild = HTMLEditUtils::GetFirstChild(
      aRemovingContainerElement, {WalkTreeOption::IgnoreNonEditableNode});
  if (!firstEditableChild) {
    return CreateElementResult::NotHandled();
  }

  if (HTMLEditUtils::IsBlockElement(*firstEditableChild) ||
      firstEditableChild->IsHTMLElement(nsGkAtoms::br)) {
    return CreateElementResult::NotHandled();
  }

  nsIContent* previousEditableContent = HTMLEditUtils::GetPreviousSibling(
      aRemovingContainerElement, {WalkTreeOption::IgnoreNonEditableNode});
  if (!previousEditableContent) {
    return CreateElementResult::NotHandled();
  }

  if (HTMLEditUtils::IsBlockElement(*previousEditableContent) ||
      previousEditableContent->IsHTMLElement(nsGkAtoms::br)) {
    return CreateElementResult::NotHandled();
  }

  CreateElementResult insertBRElementResult = InsertBRElement(
      WithTransaction::Yes, EditorDOMPoint(&aRemovingContainerElement, 0u));
  NS_WARNING_ASSERTION(
      insertBRElementResult.isOk(),
      "HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
  return insertBRElementResult;
}

CreateElementResult HTMLEditor::EnsureHardLineEndsWithLastChildOf(
    Element& aRemovingContainerElement) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  nsIContent* firstEditableContent = HTMLEditUtils::GetLastChild(
      aRemovingContainerElement, {WalkTreeOption::IgnoreNonEditableNode});
  if (!firstEditableContent) {
    return CreateElementResult::NotHandled();
  }

  if (HTMLEditUtils::IsBlockElement(*firstEditableContent) ||
      firstEditableContent->IsHTMLElement(nsGkAtoms::br)) {
    return CreateElementResult::NotHandled();
  }

  nsIContent* nextEditableContent = HTMLEditUtils::GetPreviousSibling(
      aRemovingContainerElement, {WalkTreeOption::IgnoreNonEditableNode});
  if (!nextEditableContent) {
    return CreateElementResult::NotHandled();
  }

  if (HTMLEditUtils::IsBlockElement(*nextEditableContent) ||
      nextEditableContent->IsHTMLElement(nsGkAtoms::br)) {
    return CreateElementResult::NotHandled();
  }

  CreateElementResult insertBRElementResult = InsertBRElement(
      WithTransaction::Yes, EditorDOMPoint::AtEndOf(aRemovingContainerElement));
  NS_WARNING_ASSERTION(
      insertBRElementResult.isOk(),
      "HTMLEditor::InsertBRElement(WithTransaction::Yes) failed");
  return insertBRElementResult;
}

Result<EditorDOMPoint, nsresult> HTMLEditor::SetBlockElementAlign(
    Element& aBlockOrHRElement, const nsAString& aAlignType,
    EditTarget aEditTarget) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(HTMLEditUtils::IsBlockElement(aBlockOrHRElement) ||
             aBlockOrHRElement.IsHTMLElement(nsGkAtoms::hr));
  MOZ_ASSERT(IsCSSEnabled() ||
             HTMLEditUtils::SupportsAlignAttr(aBlockOrHRElement));

  EditorDOMPoint pointToPutCaret;
  if (!aBlockOrHRElement.IsHTMLElement(nsGkAtoms::table)) {
    Result<EditorDOMPoint, nsresult> pointToPutCaretOrError =
        RemoveAlignFromDescendants(aBlockOrHRElement, aAlignType, aEditTarget);
    if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
      NS_WARNING("HTMLEditor::RemoveAlignFromDescendants() failed");
      return pointToPutCaretOrError;
    }
    if (pointToPutCaretOrError.inspect().IsSet()) {
      pointToPutCaret = pointToPutCaretOrError.unwrap();
    }
  }
  nsresult rv = SetAttributeOrEquivalent(&aBlockOrHRElement, nsGkAtoms::align,
                                         aAlignType, false);
  if (NS_WARN_IF(Destroyed())) {
    return Err(NS_ERROR_EDITOR_DESTROYED);
  }
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::SetAttributeOrEquivalent(nsGkAtoms::align) failed");
    return Err(rv);
  }
  return pointToPutCaret;
}

Result<EditorDOMPoint, nsresult> HTMLEditor::ChangeMarginStart(
    Element& aElement, ChangeMargin aChangeMargin,
    const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  nsStaticAtom& marginProperty = MarginPropertyAtomForIndent(aElement);
  if (NS_WARN_IF(Destroyed())) {
    return Err(NS_ERROR_EDITOR_DESTROYED);
  }
  nsAutoString value;
  DebugOnly<nsresult> rvIgnored =
      CSSEditUtils::GetSpecifiedProperty(aElement, marginProperty, value);
  if (NS_WARN_IF(Destroyed())) {
    return Err(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rvIgnored),
      "CSSEditUtils::GetSpecifiedProperty() failed, but ignored");
  float f;
  RefPtr<nsAtom> unit;
  CSSEditUtils::ParseLength(value, &f, getter_AddRefs(unit));
  if (!f) {
    nsAutoString defaultLengthUnit;
    CSSEditUtils::GetDefaultLengthUnit(defaultLengthUnit);
    unit = NS_Atomize(defaultLengthUnit);
  }
  int8_t multiplier = aChangeMargin == ChangeMargin::Increase ? 1 : -1;
  if (nsGkAtoms::in == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_IN * multiplier;
  } else if (nsGkAtoms::cm == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_CM * multiplier;
  } else if (nsGkAtoms::mm == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_MM * multiplier;
  } else if (nsGkAtoms::pt == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_PT * multiplier;
  } else if (nsGkAtoms::pc == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_PC * multiplier;
  } else if (nsGkAtoms::em == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_EM * multiplier;
  } else if (nsGkAtoms::ex == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_EX * multiplier;
  } else if (nsGkAtoms::px == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_PX * multiplier;
  } else if (nsGkAtoms::percentage == unit) {
    f += NS_EDITOR_INDENT_INCREMENT_PERCENT * multiplier;
  }

  if (0 < f) {
    if (nsStyledElement* styledElement = nsStyledElement::FromNode(&aElement)) {
      nsAutoString newValue;
      newValue.AppendFloat(f);
      newValue.Append(nsDependentAtomString(unit));
      // MOZ_KnownLive(*styledElement): It's aElement and its lifetime must
      // be guaranteed by caller because of MOZ_CAN_RUN_SCRIPT method.
      // MOZ_KnownLive(merginProperty): It's nsStaticAtom.
      nsresult rv = mCSSEditUtils->SetCSSPropertyWithTransaction(
          MOZ_KnownLive(*styledElement), MOZ_KnownLive(marginProperty),
          newValue);
      if (rv == NS_ERROR_EDITOR_DESTROYED) {
        NS_WARNING(
            "CSSEditUtils::SetCSSPropertyWithTransaction() destroyed the "
            "editor");
        return Err(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "CSSEditUtils::SetCSSPropertyWithTransaction() failed, but ignored");
    }
    return EditorDOMPoint();
  }

  if (nsStyledElement* styledElement = nsStyledElement::FromNode(&aElement)) {
    // MOZ_KnownLive(*styledElement): It's aElement and its lifetime must
    // be guaranteed by caller because of MOZ_CAN_RUN_SCRIPT method.
    // MOZ_KnownLive(merginProperty): It's nsStaticAtom.
    nsresult rv = mCSSEditUtils->RemoveCSSPropertyWithTransaction(
        MOZ_KnownLive(*styledElement), MOZ_KnownLive(marginProperty), value);
    if (rv == NS_ERROR_EDITOR_DESTROYED) {
      NS_WARNING(
          "CSSEditUtils::RemoveCSSPropertyWithTransaction() destroyed the "
          "editor");
      return Err(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "CSSEditUtils::RemoveCSSPropertyWithTransaction() failed, but ignored");
  }

  // Remove unnecessary divs
  if (!aElement.IsHTMLElement(nsGkAtoms::div) ||
      HTMLEditor::HasAttributes(&aElement)) {
    return EditorDOMPoint();
  }
  // Don't touch editing host nor node which is outside of it.
  if (&aElement == &aEditingHost ||
      !aElement.IsInclusiveDescendantOf(&aEditingHost)) {
    return EditorDOMPoint();
  }

  Result<EditorDOMPoint, nsresult> unwrapDivElementResult =
      RemoveContainerWithTransaction(aElement);
  NS_WARNING_ASSERTION(unwrapDivElementResult.isOk(),
                       "HTMLEditor::RemoveContainerWithTransaction() failed");
  return unwrapDivElementResult;
}

EditActionResult HTMLEditor::SetSelectionToAbsoluteAsSubAction(
    const Element& aEditingHost) {
  AutoPlaceholderBatch treatAsOneTransaction(
      *this, ScrollSelectionIntoView::Yes, __FUNCTION__);
  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this, EditSubAction::eSetPositionToAbsolute, nsIEditor::eNext,
      ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return EditActionResult(ignoredError.StealNSResult());
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (NS_SUCCEEDED(rv) && SelectionRef().IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterInvisibleBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterInvisibleBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditgor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  auto EnsureCaretInElementIfCollapsedOutside =
      [&](Element& aElement) MOZ_CAN_RUN_SCRIPT {
        if (!SelectionRef().IsCollapsed() || !SelectionRef().RangeCount()) {
          return NS_OK;
        }
        const auto firstRangeStartPoint =
            GetFirstSelectionStartPoint<EditorRawDOMPoint>();
        if (MOZ_UNLIKELY(!firstRangeStartPoint.IsSet())) {
          return NS_OK;
        }
        const Result<EditorRawDOMPoint, nsresult> pointToPutCaretOrError =
            HTMLEditUtils::ComputePointToPutCaretInElementIfOutside<
                EditorRawDOMPoint>(aElement, firstRangeStartPoint);
        if (MOZ_UNLIKELY(pointToPutCaretOrError.isErr())) {
          NS_WARNING(
              "HTMLEditUtils::ComputePointToPutCaretInElementIfOutside() "
              "failed, but ignored");
          return NS_OK;
        }
        if (!pointToPutCaretOrError.inspect().IsSet()) {
          return NS_OK;
        }
        nsresult rv = CollapseSelectionTo(pointToPutCaretOrError.inspect());
        if (MOZ_UNLIKELY(rv == NS_ERROR_EDITOR_DESTROYED)) {
          NS_WARNING("EditorBase::CollapseSelectionTo() failed");
          return NS_ERROR_EDITOR_DESTROYED;
        }
        NS_WARNING_ASSERTION(
            NS_SUCCEEDED(rv),
            "EditorBase::CollapseSelectionTo() failed, but ignored");
        return NS_OK;
      };

  RefPtr<Element> focusElement = GetSelectionContainerElement();
  if (focusElement && HTMLEditUtils::IsImage(focusElement)) {
    nsresult rv = EnsureCaretInElementIfCollapsedOutside(*focusElement);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "EnsureCaretInElementIfCollapsedOutside() failed");
    return EditActionHandled(rv);
  }

  // XXX Why do we do this only when there is only one selection range?
  if (!SelectionRef().IsCollapsed() && SelectionRef().RangeCount() == 1u) {
    Result<EditorRawDOMRange, nsresult> extendedRange =
        GetRangeExtendedToHardLineEdgesForBlockEditAction(
            SelectionRef().GetRangeAt(0u), aEditingHost);
    if (MOZ_UNLIKELY(extendedRange.isErr())) {
      NS_WARNING(
          "HTMLEditor::GetRangeExtendedToHardLineEdgesForBlockEditAction() "
          "failed");
      return EditActionResult(extendedRange.unwrapErr());
    }
    // Note that end point may be prior to start point.  So, we
    // cannot use Selection::SetStartAndEndInLimit() here.
    IgnoredErrorResult error;
    SelectionRef().SetBaseAndExtentInLimiter(
        extendedRange.inspect().StartRef().ToRawRangeBoundary(),
        extendedRange.inspect().EndRef().ToRawRangeBoundary(), error);
    if (NS_WARN_IF(Destroyed())) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    if (error.Failed()) {
      NS_WARNING("Selection::SetBaseAndExtentInLimiter() failed");
      return EditActionResult(error.StealNSResult());
    }
  }

  RefPtr<Element> divElement;
  rv = MoveSelectedContentsToDivElementToMakeItAbsolutePosition(
      address_of(divElement), aEditingHost);
  // MoveSelectedContentsToDivElementToMakeItAbsolutePosition() may restore
  // selection with AutoSelectionRestorer.  Therefore, the editor might have
  // already been destroyed now.
  if (NS_WARN_IF(Destroyed())) {
    return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
  }
  if (NS_FAILED(rv)) {
    NS_WARNING(
        "HTMLEditor::MoveSelectedContentsToDivElementToMakeItAbsolutePosition()"
        " failed");
    return EditActionHandled(rv);
  }

  if (IsSelectionRangeContainerNotContent()) {
    NS_WARNING("Mutation event listener might have changed the selection");
    return EditActionHandled(NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE);
  }

  rv = MaybeInsertPaddingBRElementForEmptyLastLineAtSelection();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return EditActionHandled(rv);
  }

  if (!divElement) {
    return EditActionHandled();
  }

  rv = SetPositionToAbsoluteOrStatic(*divElement, true);
  if (NS_WARN_IF(Destroyed())) {
    return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
  }
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::SetPositionToAbsoluteOrStatic() failed");
    return EditActionHandled(rv);
  }

  rv = EnsureCaretInElementIfCollapsedOutside(*divElement);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EnsureCaretInElementIfCollapsedOutside() failed");
  return EditActionHandled(rv);
}

nsresult HTMLEditor::MoveSelectedContentsToDivElementToMakeItAbsolutePosition(
    RefPtr<Element>* aTargetElement, const Element& aEditingHost) {
  MOZ_ASSERT(IsEditActionDataAvailable());
  MOZ_ASSERT(aTargetElement);

  AutoSelectionRestorer restoreSelectionLater(*this);

  EditorDOMPoint pointToPutCaret;

  // Use these ranges to construct a list of nodes to act on.
  AutoTArray<OwningNonNull<nsIContent>, 64> arrayOfContents;
  {
    AutoRangeArray extendedSelectionRanges(SelectionRef());
    extendedSelectionRanges.ExtendRangesToWrapLinesToHandleBlockLevelEditAction(
        EditSubAction::eSetPositionToAbsolute, aEditingHost);
    Result<EditorDOMPoint, nsresult> splitResult =
        extendedSelectionRanges
            .SplitTextNodesAtEndBoundariesAndParentInlineElementsAtBoundaries(
                *this);
    if (MOZ_UNLIKELY(splitResult.isErr())) {
      NS_WARNING(
          "AutoRangeArray::"
          "SplitTextNodesAtEndBoundariesAndParentInlineElementsAtBoundaries() "
          "failed");
      return splitResult.unwrapErr();
    }
    if (splitResult.inspect().IsSet()) {
      pointToPutCaret = splitResult.unwrap();
    }
    nsresult rv = extendedSelectionRanges.CollectEditTargetNodes(
        *this, arrayOfContents, EditSubAction::eSetPositionToAbsolute,
        AutoRangeArray::CollectNonEditableNodes::Yes);
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "AutoRangeArray::CollectEditTargetNodes(EditSubAction::"
          "eSetPositionToAbsolute, CollectNonEditableNodes::Yes) failed");
      return rv;
    }
  }

  Result<EditorDOMPoint, nsresult> splitAtBRElementsResult =
      MaybeSplitElementsAtEveryBRElement(arrayOfContents,
                                         EditSubAction::eSetPositionToAbsolute);
  if (MOZ_UNLIKELY(splitAtBRElementsResult.isErr())) {
    NS_WARNING(
        "HTMLEditor::MaybeSplitElementsAtEveryBRElement(EditSubAction::"
        "eSetPositionToAbsolute) failed");
    return splitAtBRElementsResult.inspectErr();
  }
  if (splitAtBRElementsResult.inspect().IsSet()) {
    pointToPutCaret = splitAtBRElementsResult.unwrap();
  }

  if (AllowsTransactionsToChangeSelection() &&
      pointToPutCaret.IsSetAndValid()) {
    nsresult rv = CollapseSelectionTo(pointToPutCaret);
    if (NS_FAILED(rv)) {
      NS_WARNING("EditorBase::CollapseSelectionTo() failed");
      return rv;
    }
  }

  // If there is no visible and editable nodes in the edit targets, make an
  // empty block.
  // XXX Isn't this odd if there are only non-editable visible nodes?
  if (HTMLEditUtils::IsEmptyOneHardLine(arrayOfContents)) {
    const auto atCaret =
        EditorBase::GetFirstSelectionStartPoint<EditorDOMPoint>();
    if (NS_WARN_IF(!atCaret.IsSet())) {
      return NS_ERROR_FAILURE;
    }

    // Make sure we can put a block here.
    CreateElementResult createNewDivElementResult =
        InsertElementWithSplittingAncestorsWithTransaction(
            *nsGkAtoms::div, atCaret, BRElementNextToSplitPoint::Keep,
            aEditingHost);
    if (createNewDivElementResult.isErr()) {
      NS_WARNING(
          "HTMLEditor::InsertElementWithSplittingAncestorsWithTransaction("
          "nsGkAtoms::div) failed");
      return createNewDivElementResult.unwrapErr();
    }
    // We'll update selection after deleting the content nodes and nobody
    // refers selection until then.  Therefore, we don't need to update
    // selection here.
    createNewDivElementResult.IgnoreCaretPointSuggestion();
    RefPtr<Element> newDivElement = createNewDivElementResult.UnwrapNewNode();
    MOZ_ASSERT(newDivElement);
    // Delete anything that was in the list of nodes
    // XXX We don't need to remove items from the array.
    for (OwningNonNull<nsIContent>& curNode : arrayOfContents) {
      // MOZ_KnownLive because 'arrayOfContents' is guaranteed to keep it alive.
      nsresult rv = DeleteNodeWithTransaction(MOZ_KnownLive(*curNode));
      if (NS_FAILED(rv)) {
        NS_WARNING("EditorBase::DeleteNodeWithTransaction() failed");
        return rv;
      }
    }
    // Don't restore the selection
    restoreSelectionLater.Abort();
    nsresult rv = CollapseSelectionToStartOf(*newDivElement);
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "EditorBase::CollapseSelectionToStartOf() failed");
    *aTargetElement = std::move(newDivElement);
    return rv;
  }

  // `<div>` element to be positioned absolutely.  This may have already
  // existed or newly created by this method.
  RefPtr<Element> targetDivElement;
  // Newly created list element for moving selected list item elements into
  // targetDivElement.  I.e., this is created in the `<div>` element.
  RefPtr<Element> createdListElement;
  // If we handle a parent list item element, this is set to it.  In such case,
  // we should handle its children again.
  RefPtr<Element> handledListItemElement;
  for (OwningNonNull<nsIContent>& content : arrayOfContents) {
    // Here's where we actually figure out what to do.
    EditorDOMPoint atContent(content);
    if (NS_WARN_IF(!atContent.IsSet())) {
      return NS_ERROR_FAILURE;  // XXX not continue??
    }

    // Ignore all non-editable nodes.  Leave them be.
    if (!EditorUtils::IsEditableContent(content, EditorType::HTML)) {
      continue;
    }

    // If current node is a child of a list element, we need another list
    // element in absolute-positioned `<div>` element to avoid non-selected
    // list items are moved into the `<div>` element.
    if (HTMLEditUtils::IsAnyListElement(atContent.GetContainer())) {
      // If we cannot move current node to created list element, we need a
      // list element in the target `<div>` element for the destination.
      // Therefore, duplicate same list element into the target `<div>`
      // element.
      nsIContent* previousEditableContent =
          createdListElement
              ? HTMLEditUtils::GetPreviousSibling(
                    content, {WalkTreeOption::IgnoreNonEditableNode})
              : nullptr;
      if (!createdListElement ||
          (previousEditableContent &&
           previousEditableContent != createdListElement)) {
        nsAtom* ULOrOLOrDLTagName =
            atContent.GetContainer()->NodeInfo()->NameAtom();
        if (targetDivElement) {
          // XXX Do we need to split the container? Since we'll append new
          //     element at end of the <div> element.
          const SplitNodeResult splitNodeResult =
              MaybeSplitAncestorsForInsertWithTransaction(
                  MOZ_KnownLive(*ULOrOLOrDLTagName), atContent, aEditingHost);
          if (splitNodeResult.isOk()) {
            NS_WARNING(
                "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction() "
                "failed");
            return splitNodeResult.unwrapErr();
          }
          // We'll update selection after creating a list element below.
          // Therefore, we don't need to touch selection here.
          splitNodeResult.IgnoreCaretPointSuggestion();
        } else {
          // If we've not had a target <div> element yet, let's insert a <div>
          // element with splitting the ancestors.
          CreateElementResult createNewDivElementResult =
              InsertElementWithSplittingAncestorsWithTransaction(
                  *nsGkAtoms::div, atContent, BRElementNextToSplitPoint::Keep,
                  aEditingHost);
          if (createNewDivElementResult.isErr()) {
            NS_WARNING(
                "HTMLEditor::"
                "InsertElementWithSplittingAncestorsWithTransaction(nsGkAtoms::"
                "div) failed");
            return createNewDivElementResult.unwrapErr();
          }
          // We'll update selection after creating a list element below.
          // Therefor, we don't need to touch selection here.
          createNewDivElementResult.IgnoreCaretPointSuggestion();
          MOZ_ASSERT(createNewDivElementResult.GetNewNode());
          targetDivElement = createNewDivElementResult.UnwrapNewNode();
        }
        CreateElementResult createNewListElementResult = CreateAndInsertElement(
            WithTransaction::Yes, MOZ_KnownLive(*ULOrOLOrDLTagName),
            EditorDOMPoint::AtEndOf(targetDivElement));
        if (createNewListElementResult.isErr()) {
          NS_WARNING(
              "HTMLEditor::CreateAndInsertElement(WithTransaction::Yes) "
              "failed");
          return createNewListElementResult.unwrapErr();
        }
        nsresult rv = createNewListElementResult.SuggestCaretPointTo(
            *this, {SuggestCaret::OnlyIfHasSuggestion,
                    SuggestCaret::OnlyIfTransactionsAllowedToDoIt,
                    SuggestCaret::AndIgnoreTrivialError});
        if (NS_FAILED(rv)) {
          NS_WARNING("CreateElementResult::SuggestCaretPointTo() failed");
          return Err(rv);
        }
        NS_WARNING_ASSERTION(
            rv != NS_SUCCESS_EDITOR_BUT_IGNORED_TRIVIAL_ERROR,
            "CreateElementResult::SuggestCaretPointTo() failed, but ignored");
        createdListElement = createNewListElementResult.UnwrapNewNode();
        MOZ_ASSERT(createdListElement);
      }
      // Move current node (maybe, assumed as a list item element) into the
      // new list element in the target `<div>` element to be positioned
      // absolutely.
      // MOZ_KnownLive because 'arrayOfContents' is guaranteed to keep it alive.
      const MoveNodeResult moveNodeResult = MoveNodeToEndWithTransaction(
          MOZ_KnownLive(content), *createdListElement);
      if (moveNodeResult.isErr()) {
        NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
        return Err(moveNodeResult.unwrapErr());
      }
      nsresult rv = moveNodeResult.SuggestCaretPointTo(
          *this, {SuggestCaret::OnlyIfHasSuggestion,
                  SuggestCaret::OnlyIfTransactionsAllowedToDoIt,
                  SuggestCaret::AndIgnoreTrivialError});
      if (NS_FAILED(rv)) {
        NS_WARNING("MoveNodeResult::SuggestCaretPointTo() failed");
        return Err(rv);
      }
      NS_WARNING_ASSERTION(
          rv != NS_SUCCESS_EDITOR_BUT_IGNORED_TRIVIAL_ERROR,
          "MoveNodeResult::SuggestCaretPointTo() failed, but ignored");
      continue;
    }

    // If contents in a list item element is selected, we should move current
    // node into the target `<div>` element with the list item element itself
    // because we want to keep indent level of the contents.
    if (RefPtr<Element> listItemElement =
            HTMLEditUtils::GetClosestAncestorListItemElement(content,
                                                             &aEditingHost)) {
      if (handledListItemElement == listItemElement) {
        // Current node has already been moved into the `<div>` element.
        continue;
      }
      // If we cannot move the list item element into created list element,
      // we need another list element in the target `<div>` element.
      nsIContent* previousEditableContent =
          createdListElement
              ? HTMLEditUtils::GetPreviousSibling(
                    *listItemElement, {WalkTreeOption::IgnoreNonEditableNode})
              : nullptr;
      if (!createdListElement ||
          (previousEditableContent &&
           previousEditableContent != createdListElement)) {
        EditorDOMPoint atListItem(listItemElement);
        if (NS_WARN_IF(!atListItem.IsSet())) {
          return NS_ERROR_FAILURE;
        }
        // XXX If content is the listItemElement and not in a list element,
        //     we duplicate wrong element into the target `<div>` element.
        nsAtom* containerName =
            atListItem.GetContainer()->NodeInfo()->NameAtom();
        if (targetDivElement) {
          // XXX Do we need to split the container? Since we'll append new
          //     element at end of the <div> element.
          const SplitNodeResult splitNodeResult =
              MaybeSplitAncestorsForInsertWithTransaction(
                  MOZ_KnownLive(*containerName), atListItem, aEditingHost);
          if (splitNodeResult.isErr()) {
            NS_WARNING(
                "HTMLEditor::MaybeSplitAncestorsForInsertWithTransaction() "
                "failed");
            return splitNodeResult.unwrapErr();
          }
          // We'll update selection after creating a list element below.
          // Therefore, we don't need to touch selection here.
          splitNodeResult.IgnoreCaretPointSuggestion();
        } else {
          // If we've not had a target <div> element yet, let's insert a <div>
          // element with splitting the ancestors.
          CreateElementResult createNewDivElementResult =
              InsertElementWithSplittingAncestorsWithTransaction(
                  *nsGkAtoms::div, atContent, BRElementNextToSplitPoint::Keep,
                  aEditingHost);
          if (createNewDivElementResult.isErr()) {
            NS_WARNING(
                "HTMLEditor::"
                "InsertElementWithSplittingAncestorsWithTransaction("
                "nsGkAtoms::div) failed");
            return createNewDivElementResult.unwrapErr();
          }
          // We'll update selection after creating a list element below.
          // Therefore, we don't need to touch selection here.
          createNewDivElementResult.IgnoreCaretPointSuggestion();
          MOZ_ASSERT(createNewDivElementResult.GetNewNode());
          targetDivElement = createNewDivElementResult.UnwrapNewNode();
        }
        // XXX So, createdListElement may be set to a non-list element.
        CreateElementResult createNewListElementResult = CreateAndInsertElement(
            WithTransaction::Yes, MOZ_KnownLive(*containerName),
            EditorDOMPoint::AtEndOf(targetDivElement));
        if (createNewListElementResult.isErr()) {
          NS_WARNING(
              "HTMLEditor::CreateAndInsertElement(WithTransaction::Yes) "
              "failed");
          return createNewListElementResult.unwrapErr();
        }
        nsresult rv = createNewListElementResult.SuggestCaretPointTo(
            *this, {SuggestCaret::OnlyIfHasSuggestion,
                    SuggestCaret::OnlyIfTransactionsAllowedToDoIt,
                    SuggestCaret::AndIgnoreTrivialError});
        if (NS_FAILED(rv)) {
          NS_WARNING("CreateElementResult::SuggestCaretPointTo() failed");
          return Err(rv);
        }
        NS_WARNING_ASSERTION(
            rv != NS_SUCCESS_EDITOR_BUT_IGNORED_TRIVIAL_ERROR,
            "CreateElementResult::SuggestCaretPointTo() failed, but ignored");
        createdListElement = createNewListElementResult.UnwrapNewNode();
        MOZ_ASSERT(createdListElement);
      }
      // Move current list item element into the createdListElement (could be
      // non-list element due to the above bug) in a candidate `<div>` element
      // to be positioned absolutely.
      const MoveNodeResult moveListItemElementResult =
          MoveNodeToEndWithTransaction(*listItemElement, *createdListElement);
      if (moveListItemElementResult.isErr()) {
        NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
        return Err(moveListItemElementResult.unwrapErr());
      }
      nsresult rv = moveListItemElementResult.SuggestCaretPointTo(
          *this, {SuggestCaret::OnlyIfHasSuggestion,
                  SuggestCaret::OnlyIfTransactionsAllowedToDoIt,
                  SuggestCaret::AndIgnoreTrivialError});
      if (NS_FAILED(rv)) {
        NS_WARNING("MoveNodeResult::SuggestCaretPointTo() failed");
        return Err(rv);
      }
      NS_WARNING_ASSERTION(
          rv != NS_SUCCESS_EDITOR_BUT_IGNORED_TRIVIAL_ERROR,
          "MoveNodeResult::SuggestCaretPointTo() failed, but ignored");
      handledListItemElement = std::move(listItemElement);
      continue;
    }

    if (!targetDivElement) {
      // If we meet a `<div>` element, use it as the absolute-position
      // container.
      // XXX This looks odd.  If there are 2 or more `<div>` elements are
      //     selected, first found `<div>` element will have all other
      //     selected nodes.
      if (content->IsHTMLElement(nsGkAtoms::div)) {
        targetDivElement = content->AsElement();
        MOZ_ASSERT(!createdListElement);
        MOZ_ASSERT(!handledListItemElement);
        continue;
      }
      // Otherwise, create new `<div>` element to be positioned absolutely
      // and to contain all selected nodes.
      CreateElementResult createNewDivElementResult =
          InsertElementWithSplittingAncestorsWithTransaction(
              *nsGkAtoms::div, atContent, BRElementNextToSplitPoint::Keep,
              aEditingHost);
      if (createNewDivElementResult.isErr()) {
        NS_WARNING(
            "HTMLEditor::InsertElementWithSplittingAncestorsWithTransaction("
            "nsGkAtoms::div) failed");
        return createNewDivElementResult.unwrapErr();
      }
      nsresult rv = createNewDivElementResult.SuggestCaretPointTo(
          *this, {SuggestCaret::OnlyIfHasSuggestion,
                  SuggestCaret::OnlyIfTransactionsAllowedToDoIt});
      if (NS_FAILED(rv)) {
        NS_WARNING("CreateElementResult::SuggestCaretPointTo() failed");
        return rv;
      }
      MOZ_ASSERT(createNewDivElementResult.GetNewNode());
      targetDivElement = createNewDivElementResult.UnwrapNewNode();
    }

    // MOZ_KnownLive because 'arrayOfContents' is guaranteed to keep it alive.
    const MoveNodeResult moveNodeResult =
        MoveNodeToEndWithTransaction(MOZ_KnownLive(content), *targetDivElement);
    if (moveNodeResult.isErr()) {
      NS_WARNING("HTMLEditor::MoveNodeToEndWithTransaction() failed");
      return moveNodeResult.unwrapErr();
    }
    nsresult rv = moveNodeResult.SuggestCaretPointTo(
        *this, {SuggestCaret::OnlyIfHasSuggestion,
                SuggestCaret::OnlyIfTransactionsAllowedToDoIt,
                SuggestCaret::AndIgnoreTrivialError});
    if (NS_FAILED(rv)) {
      NS_WARNING("MoveNodeResult::SuggestCaretPointTo() failed");
      return rv;
    }
    NS_WARNING_ASSERTION(
        rv != NS_SUCCESS_EDITOR_BUT_IGNORED_TRIVIAL_ERROR,
        "MoveNodeResult::SuggestCaretPointTo() failed, but ignored");
    // Forget createdListElement, if any
    createdListElement = nullptr;
  }
  *aTargetElement = std::move(targetDivElement);
  return NS_OK;
}

EditActionResult HTMLEditor::SetSelectionToStaticAsSubAction() {
  MOZ_ASSERT(IsEditActionDataAvailable());

  AutoPlaceholderBatch treatAsOneTransaction(
      *this, ScrollSelectionIntoView::Yes, __FUNCTION__);
  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this, EditSubAction::eSetPositionToStatic, nsIEditor::eNext,
      ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return EditActionResult(ignoredError.StealNSResult());
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (NS_SUCCEEDED(rv) && SelectionRef().IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterInvisibleBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterInvisibleBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionResult(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  RefPtr<Element> element = GetAbsolutelyPositionedSelectionContainer();
  if (!element) {
    if (NS_WARN_IF(Destroyed())) {
      return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING(
        "HTMLEditor::GetAbsolutelyPositionedSelectionContainer() returned "
        "nullptr");
    return EditActionHandled(NS_ERROR_FAILURE);
  }

  {
    AutoSelectionRestorer restoreSelectionLater(*this);

    nsresult rv = SetPositionToAbsoluteOrStatic(*element, false);
    if (NS_WARN_IF(Destroyed())) {
      return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
    }
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::SetPositionToAbsoluteOrStatic() failed");
      return EditActionHandled(rv);
    }
  }

  // Restoring Selection might cause destroying the HTML editor.
  return NS_WARN_IF(Destroyed()) ? EditActionHandled(NS_ERROR_EDITOR_DESTROYED)
                                 : EditActionHandled(NS_OK);
}

EditActionResult HTMLEditor::AddZIndexAsSubAction(int32_t aChange) {
  MOZ_ASSERT(IsEditActionDataAvailable());

  AutoPlaceholderBatch treatAsOneTransaction(
      *this, ScrollSelectionIntoView::Yes, __FUNCTION__);
  IgnoredErrorResult ignoredError;
  AutoEditSubActionNotifier startToHandleEditSubAction(
      *this,
      aChange < 0 ? EditSubAction::eDecreaseZIndex
                  : EditSubAction::eIncreaseZIndex,
      nsIEditor::eNext, ignoredError);
  if (NS_WARN_IF(ignoredError.ErrorCodeIs(NS_ERROR_EDITOR_DESTROYED))) {
    return EditActionResult(ignoredError.StealNSResult());
  }
  NS_WARNING_ASSERTION(
      !ignoredError.Failed(),
      "HTMLEditor::OnStartToHandleTopLevelEditSubAction() failed, but ignored");

  EditActionResult result = CanHandleHTMLEditSubAction();
  if (result.Failed() || result.Canceled()) {
    NS_WARNING_ASSERTION(result.Succeeded(),
                         "HTMLEditor::CanHandleHTMLEditSubAction() failed");
    return result;
  }

  nsresult rv = EnsureNoPaddingBRElementForEmptyEditor();
  if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
    return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
  }
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "EditorBase::EnsureNoPaddingBRElementForEmptyEditor() "
                       "failed, but ignored");

  if (NS_SUCCEEDED(rv) && SelectionRef().IsCollapsed()) {
    nsresult rv = EnsureCaretNotAfterInvisibleBRElement();
    if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
      return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "HTMLEditor::EnsureCaretNotAfterInvisibleBRElement() "
                         "failed, but ignored");
    if (NS_SUCCEEDED(rv)) {
      nsresult rv = PrepareInlineStylesForCaret();
      if (NS_WARN_IF(rv == NS_ERROR_EDITOR_DESTROYED)) {
        return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
      }
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "HTMLEditor::PrepareInlineStylesForCaret() failed, but ignored");
    }
  }

  RefPtr<Element> absolutelyPositionedElement =
      GetAbsolutelyPositionedSelectionContainer();
  if (!absolutelyPositionedElement) {
    if (NS_WARN_IF(Destroyed())) {
      return EditActionHandled(NS_ERROR_EDITOR_DESTROYED);
    }
    NS_WARNING(
        "HTMLEditor::GetAbsolutelyPositionedSelectionContainer() returned "
        "nullptr");
    return EditActionHandled(NS_ERROR_FAILURE);
  }

  nsStyledElement* absolutelyPositionedStyledElement =
      nsStyledElement::FromNode(absolutelyPositionedElement);
  if (NS_WARN_IF(!absolutelyPositionedStyledElement)) {
    return EditActionHandled(NS_ERROR_FAILURE);
  }

  {
    AutoSelectionRestorer restoreSelectionLater(*this);

    // MOZ_KnownLive(*absolutelyPositionedStyledElement): It's
    // absolutelyPositionedElement whose type is RefPtr.
    Result<int32_t, nsresult> result = AddZIndexWithTransaction(
        MOZ_KnownLive(*absolutelyPositionedStyledElement), aChange);
    if (result.isErr()) {
      NS_WARNING("HTMLEditor::AddZIndexWithTransaction() failed");
      return EditActionHandled(result.unwrapErr());
    }
  }

  // Restoring Selection might cause destroying the HTML editor.
  return NS_WARN_IF(Destroyed()) ? EditActionHandled(NS_ERROR_EDITOR_DESTROYED)
                                 : EditActionHandled(NS_OK);
}

nsresult HTMLEditor::OnDocumentModified() {
  if (mPendingDocumentModifiedRunner) {
    return NS_OK;  // We've already posted same runnable into the queue.
  }
  mPendingDocumentModifiedRunner = NewRunnableMethod(
      "HTMLEditor::OnModifyDocument", this, &HTMLEditor::OnModifyDocument);
  nsContentUtils::AddScriptRunner(do_AddRef(mPendingDocumentModifiedRunner));
  // Be aware, if OnModifyDocument() may be called synchronously, the
  // editor might have been destroyed here.
  return NS_WARN_IF(Destroyed()) ? NS_ERROR_EDITOR_DESTROYED : NS_OK;
}

}  // namespace mozilla
