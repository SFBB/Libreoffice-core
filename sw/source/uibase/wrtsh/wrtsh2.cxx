/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#include <svl/macitem.hxx>
#include <sfx2/frame.hxx>
#include <svl/eitem.hxx>
#include <svl/listener.hxx>
#include <svl/stritem.hxx>
#include <sfx2/docfile.hxx>
#include <sfx2/dispatch.hxx>
#include <sfx2/linkmgr.hxx>
#include <sfx2/viewfrm.hxx>
#include <sot/exchange.hxx>
#include <osl/diagnose.h>
#include <o3tl/string_view.hxx>
#include <fmtinfmt.hxx>
#include <wrtsh.hxx>
#include <docsh.hxx>
#include <fldbas.hxx>
#include <expfld.hxx>
#include <docufld.hxx>
#include <reffld.hxx>
#include <swundo.hxx>
#include <doc.hxx>
#include <frmfmt.hxx>
#include <fmtfld.hxx>
#include <view.hxx>
#include <swevent.hxx>
#include <section.hxx>
#include <navicont.hxx>
#include <txtinet.hxx>
#include <cmdid.h>
#include <swabstdlg.hxx>
#include <SwRewriter.hxx>
#include <authfld.hxx>
#include <ndtxt.hxx>

#include <com/sun/star/document/XDocumentProperties.hpp>
#include <com/sun/star/document/XDocumentPropertiesSupplier.hpp>

#include <memory>

#include <LibreOfficeKit/LibreOfficeKitEnums.h>
#include <comphelper/lok.hxx>
#include <sfx2/event.hxx>
#include <sal/log.hxx>

bool SwWrtShell::InsertField2(SwField const& rField, SwPaM* pAnnotationRange)
{
    ResetCursorStack();
    if(!CanInsert())
        return false;
    StartAllAction();

    SwRewriter aRewriter;
    aRewriter.AddRule(UndoArg1, rField.GetDescription());

    StartUndo(SwUndoId::INSERT, &aRewriter);

    bool bDeleted = false;
    std::optional<SwPaM> pAnnotationTextRange;
    if (pAnnotationRange)
    {
        pAnnotationTextRange.emplace(*pAnnotationRange->Start(), *pAnnotationRange->End());
    }

    if ( HasSelection() )
    {
        if ( rField.GetTyp()->Which() == SwFieldIds::Postit )
        {
            // for annotation fields:
            // - keep the current selection in order to create a corresponding annotation mark
            // - collapse cursor to its end
            if ( IsTableMode() )
            {
                GetTableCrs()->Normalize( false );
                const SwPosition rStartPos( *(GetTableCrs()->GetMark()->GetNode().GetContentNode()), 0 );
                KillPams();
                if ( !IsEndOfPara() )
                {
                    EndPara();
                }
                const SwPosition rEndPos( *GetCurrentShellCursor().GetPoint() );
                pAnnotationTextRange.emplace( rStartPos, rEndPos );
            }
            else
            {
                NormalizePam( false );
                const SwPaM& rCurrPaM = GetCurrentShellCursor();
                pAnnotationTextRange.emplace( *rCurrPaM.GetPoint(), *rCurrPaM.GetMark() );
                ClearMark();
            }
        }
        else
        {
            bDeleted = DelRight();
        }
    }

    bool const isSuccess = SwEditShell::InsertField(rField, bDeleted);

    if ( pAnnotationTextRange )
    {
        if ( GetDoc() != nullptr )
        {
            const SwPaM& rCurrPaM = GetCurrentShellCursor();
            if (*rCurrPaM.Start() == *pAnnotationTextRange->Start()
                && *rCurrPaM.End() == *pAnnotationTextRange->End())
            {
                // Annotation range was passed in externally, and inserting the postit field shifted
                // its start/end positions right by one. Restore the original position for the range
                // start. This allows commenting on the placeholder character of the field.
                if (pAnnotationTextRange->Start()->GetContentIndex() > 0)
                    pAnnotationTextRange->Start()->AdjustContent(-1);
            }
            IDocumentMarkAccess* pMarksAccess = GetDoc()->getIDocumentMarkAccess();
            pMarksAccess->makeAnnotationMark( *pAnnotationTextRange, OUString() );
        }
        pAnnotationTextRange.reset();
    }

    EndUndo();
    EndAllAction();

    return isSuccess;
}

// Start the field update

void SwWrtShell::UpdateInputFields( SwInputFieldList* pLst )
{
    // Go through the list of fields and updating
    std::unique_ptr<SwInputFieldList> pTmp;
    if (!pLst)
    {
        pTmp.reset(new SwInputFieldList( this ));
        pLst = pTmp.get();
    }

    const size_t nCnt = pLst->Count();
    if(!nCnt)
        return;

    pLst->PushCursor();

    bool bCancel = false;

    size_t nIndex = 0;
    FieldDialogPressedButton ePressedButton = FieldDialogPressedButton::NONE;

    SwField* pField = GetCurField();
    if (pField)
    {
        for (size_t i = 0; i < nCnt; i++)
        {
            if (pField == pLst->GetField(i))
            {
                nIndex = i;
                break;
            }
        }
    }

    while (!bCancel)
    {
        bool bPrev = nIndex > 0;
        bool bNext = nIndex < nCnt - 1;
        pLst->GotoFieldPos(nIndex);
        pField = pLst->GetField(nIndex);
        if (pField->GetTyp()->Which() == SwFieldIds::Dropdown)
        {
            bCancel = StartDropDownFieldDlg(pField, bPrev, bNext, GetView().GetFrameWeld(), &ePressedButton);
        }
        else
            bCancel = StartInputFieldDlg(pField, bPrev, bNext, GetView().GetFrameWeld(), &ePressedButton);

        if (!bCancel)
        {
            // Otherwise update error at multi-selection:
            pLst->GetField(nIndex)->GetTyp()->UpdateFields();

            if (ePressedButton == FieldDialogPressedButton::Previous && nIndex > 0)
                nIndex--;
            else if (ePressedButton == FieldDialogPressedButton::Next && nIndex < nCnt - 1)
                nIndex++;
            else
                bCancel = true;
        }
    }

    pLst->PopCursor();
}

namespace {

// Listener class: will close InputField dialog if input field(s)
// is(are) deleted (for instance, by an extension) after the dialog shows up.
// Otherwise, the for loop in SwWrtShell::UpdateInputFields will crash when doing:
//         'pTmp->GetField( i )->GetTyp()->UpdateFields();'
// on a deleted field.
class FieldDeletionListener : public SvtListener
{
    public:
        FieldDeletionListener(AbstractFieldInputDlg* pInputFieldDlg, SwField* pField)
            : mpInputFieldDlg(pInputFieldDlg)
            , mpFormatField(nullptr)
        {
            SwInputField *const pInputField(dynamic_cast<SwInputField*>(pField));
            SwSetExpField *const pSetExpField(dynamic_cast<SwSetExpField*>(pField));

            if (pInputField && pInputField->GetFormatField())
            {
                mpFormatField = pInputField->GetFormatField();
            }
            else if (pSetExpField && pSetExpField->GetFormatField())
            {
                mpFormatField = pSetExpField->GetFormatField();
            }

            // Register for possible field deletion while dialog is open
            if (mpFormatField)
                StartListening(mpFormatField->GetNotifier());
        }

        virtual ~FieldDeletionListener() override
        {
            // Dialog closed, remove modification listener
            EndListeningAll();
        }

        virtual void Notify(const SfxHint& rHint) override
        {
            // Input field has been deleted: better to close the dialog
            if(rHint.GetId() == SfxHintId::Dying)
            {
                mpFormatField = nullptr;
                mpInputFieldDlg->EndDialog(RET_CANCEL);
            }
        }
    private:
        VclPtr<AbstractFieldInputDlg> mpInputFieldDlg;
        SwFormatField* mpFormatField;
};

}

// Start input dialog for a specific field
bool SwWrtShell::StartInputFieldDlg(SwField* pField, bool bPrevButton, bool bNextButton,
                                    weld::Widget* pParentWin, SwWrtShell::FieldDialogPressedButton* pPressedButton)
{

    SwAbstractDialogFactory* pFact = SwAbstractDialogFactory::Create();
    ScopedVclPtr<AbstractFieldInputDlg> pDlg(pFact->CreateFieldInputDlg(pParentWin, *this, pField, bPrevButton, bNextButton));

    bool bRet;

    {
        FieldDeletionListener aModify(pDlg.get(), pField);
        bRet = RET_CANCEL == pDlg->Execute();
    }

    if (pPressedButton)
    {
        if (pDlg->PrevButtonPressed())
            *pPressedButton = FieldDialogPressedButton::Previous;
        else if (pDlg->NextButtonPressed())
            *pPressedButton = FieldDialogPressedButton::Next;
    }

    pDlg.disposeAndClear();
    GetWin()->PaintImmediately();
    return bRet;
}

bool SwWrtShell::StartDropDownFieldDlg(SwField* pField, bool bPrevButton, bool bNextButton,
                                       weld::Widget* pParentWin, SwWrtShell::FieldDialogPressedButton* pPressedButton)
{
    SwAbstractDialogFactory* pFact = SwAbstractDialogFactory::Create();
    ScopedVclPtr<AbstractDropDownFieldDialog> pDlg(pFact->CreateDropDownFieldDialog(pParentWin, *this, pField, bPrevButton, bNextButton));
    const short nRet = pDlg->Execute();

    if (pPressedButton)
    {
        if (pDlg->PrevButtonPressed())
            *pPressedButton = FieldDialogPressedButton::Previous;
        else if (pDlg->NextButtonPressed())
            *pPressedButton = FieldDialogPressedButton::Next;
    }

    pDlg.disposeAndClear();
    bool bRet = RET_CANCEL == nRet;
    GetWin()->PaintImmediately();
    if(RET_YES == nRet)
    {
        GetView().GetViewFrame().GetDispatcher()->Execute(FN_EDIT_FIELD, SfxCallMode::SYNCHRON);
    }
    return bRet;
}

// Insert directory - remove selection

void SwWrtShell::InsertTableOf(const SwTOXBase& rTOX, const SfxItemSet* pSet)
{
    if(!CanInsert())
        return;

    if(HasSelection())
        DelRight();

    SwEditShell::InsertTableOf(rTOX, pSet);
}

// Update directory - remove selection

void SwWrtShell::UpdateTableOf(const SwTOXBase& rTOX, const SfxItemSet* pSet)
{
    if(CanInsert())
    {
        SwEditShell::UpdateTableOf(rTOX, pSet);
    }
}

// handler for click on the field given as parameter.
// the cursor is positioned on the field.

void SwWrtShell::ClickToField(const SwField& rField, bool bExecHyperlinks)
{
    addCurrentPosition();

    // Since the cross reference and bibliography mark move the cursor,
    //  only select the field if it's not a Ctrl+Click
    if (!bExecHyperlinks
        || (SwFieldIds::GetRef != rField.GetTyp()->Which()
            && SwFieldIds::TableOfAuthorities != rField.GetTyp()->Which()))
    {
        StartAllAction();
        Right( SwCursorSkipMode::Chars, true, 1, false ); // Select the field.
        NormalizePam();
        EndAllAction();
    }

    m_bIsInClickToEdit = true;
    switch( rField.GetTyp()->Which() )
    {
    case SwFieldIds::JumpEdit:
        {
            sal_uInt16 nSlotId = 0;
            switch( rField.GetFormat() )
            {
            case JE_FMT_TABLE:
                nSlotId = FN_INSERT_TABLE;
                break;

            case JE_FMT_FRAME:
                nSlotId = FN_INSERT_FRAME;
                break;

            case JE_FMT_GRAPHIC:    nSlotId = SID_INSERT_GRAPHIC;       break;
            case JE_FMT_OLE:        nSlotId = SID_INSERT_OBJECT;        break;

            }

            if( nSlotId )
            {
                StartUndo( SwUndoId::START );
                //#97295# immediately select the right shell
                GetView().StopShellTimer();
                GetView().GetViewFrame().GetDispatcher()->Execute( nSlotId,
                            SfxCallMode::SYNCHRON|SfxCallMode::RECORD );
                EndUndo( SwUndoId::END );
            }
        }
        break;

    case SwFieldIds::Macro:
        {
            const SwMacroField *pField = static_cast<const SwMacroField*>(&rField);
            const OUString sText( rField.GetPar2() );
            OUString sRet( sText );
            ExecMacro( pField->GetSvxMacro(), &sRet );

            // return value changed?
            if( sRet != sText )
            {
                StartAllAction();
                const_cast<SwField&>(rField).SetPar2( sRet );
                rField.GetTyp()->UpdateFields();
                EndAllAction();
            }
        }
        break;

    case SwFieldIds::TableOfAuthorities:
        {
            if (!bExecHyperlinks)
                break; // Since it's not a Ctrl+Click, do not jump anywhere

            Point vStartPoint = GetCursor_()->GetPtPos();
            const SwAuthorityField* pField = static_cast<const SwAuthorityField*>(&rField);

            if (auto targetType = pField->GetTargetType();
                targetType == SwAuthorityField::TargetType::UseDisplayURL
                || targetType == SwAuthorityField::TargetType::UseTargetURL)
            {
                // Since the user selected target type with URL, try to use it if not empty
                if (const OUString& rURL = pField->GetAbsoluteURL();
                    rURL.getLength() > 0)
                    ::LoadURL(*this, rURL, LoadUrlFlags::NewView, /*rTargetFrameName=*/OUString());
            }
            else if (targetType == SwAuthorityField::TargetType::BibliographyTableRow)
            {
                // Since the user selected to target Bibliography Table Row,
                //  try finding matching bibliography table line

                const bool bWasViewLocked = IsViewLocked();
                LockView(true);

                // Note: This way of iterating doesn't seem to take into account TOXes
                //          that are in a frame, probably in some other cases too
                GotoPage(1);
                while (GotoNextTOXBase())
                {
                    const SwTOXBase* pIteratedTOX = nullptr;
                    const SwTOXBase* pPreviousTOX = nullptr;
                    OUString vFieldText;
                    while ((pIteratedTOX = GetCurTOX()) != nullptr
                           && pIteratedTOX->GetType() == TOX_AUTHORITIES)
                    {
                        if (pIteratedTOX != pPreviousTOX)
                            vFieldText = pField->GetAuthority(GetLayout(), &pIteratedTOX->GetTOXForm());

                        if (const SwNode& rCurrentNode = GetCursor()->GetPoint()->GetNode();
                            rCurrentNode.GetNodeType() == SwNodeType::Text
                            && (GetCursor()->GetPoint()->GetNode().FindSectionNode()->GetSection().GetType()
                                == SectionType::ToxContent) // this checks it's not a heading
                            && static_cast<const SwTextNode*>(&rCurrentNode)->GetText() == vFieldText)
                        {
                            // Since a node has been found that is a text node, isn't a heading,
                            //  and has text matching to text generated by the field, jump to it
                            LockView(bWasViewLocked);
                            ShowCursor();
                            return;
                        }
                        pPreviousTOX = pIteratedTOX;
                        FwdPara();
                    }
                }
                // Since a matching node has not been found, return to original position
                SetCursor(&vStartPoint);
                LockView(bWasViewLocked);
            }
        }
        break;

    case SwFieldIds::GetRef:
        if (!bExecHyperlinks)
            break;

        StartAllAction();
        SwCursorShell::GotoRefMark( static_cast<const SwGetRefField&>(rField).GetSetRefName(),
                                    static_cast<const SwGetRefField&>(rField).GetSubType(),
                                    static_cast<const SwGetRefField&>(rField).GetSeqNo(),
                                    static_cast<const SwGetRefField&>(rField).GetFlags() );
        EndAllAction();
        break;

    case SwFieldIds::Input:
        {
            const SwInputField* pInputField = dynamic_cast<const SwInputField*>(&rField);
            if ( pInputField == nullptr )
            {
                StartInputFieldDlg(const_cast<SwField*>(&rField), false, false, GetView().GetFrameWeld());
            }
        }
        break;

    case SwFieldIds::SetExp:
        if( static_cast<const SwSetExpField&>(rField).GetInputFlag() )
            StartInputFieldDlg(const_cast<SwField*>(&rField), false, false, GetView().GetFrameWeld());
        break;
    case SwFieldIds::Dropdown :
        StartDropDownFieldDlg(const_cast<SwField*>(&rField), false, false, GetView().GetFrameWeld());
    break;
    default:
        SAL_WARN_IF(rField.IsClickable(), "sw", "unhandled clickable field!");
    }

    m_bIsInClickToEdit = false;
}

void SwWrtShell::ClickToINetAttr( const SwFormatINetFormat& rItem, LoadUrlFlags nFilter )
{
    addCurrentPosition();

    if( rItem.GetValue().isEmpty() )
        return ;

    m_bIsInClickToEdit = true;

    // At first run the possibly set ObjectSelect Macro
    const SvxMacro* pMac = rItem.GetMacro( SvMacroItemId::OnClick );
    if( pMac )
    {
        SwCallMouseEvent aCallEvent;
        aCallEvent.Set( &rItem );
        GetDoc()->CallEvent( SvMacroItemId::OnClick, aCallEvent );
    }

    // So that the implementation of templates is displayed immediately
    ::LoadURL( *this, rItem.GetValue(), nFilter, rItem.GetTargetFrame() );
    const SwTextINetFormat* pTextAttr = rItem.GetTextINetFormat();
    if( pTextAttr )
    {
        const_cast<SwTextINetFormat*>(pTextAttr)->SetVisited( true );
        const_cast<SwTextINetFormat*>(pTextAttr)->SetVisitedValid( true );
    }

    m_bIsInClickToEdit = false;
}

bool SwWrtShell::ClickToINetGrf( const Point& rDocPt, LoadUrlFlags nFilter )
{
    bool bRet = false;
    OUString sURL;
    OUString sTargetFrameName;
    const SwFrameFormat* pFnd = IsURLGrfAtPos( rDocPt, &sURL, &sTargetFrameName );
    if( pFnd && !sURL.isEmpty() )
    {
        bRet = true;
        // At first run the possibly set ObjectSelect Macro
        SwCallMouseEvent aCallEvent;
        aCallEvent.Set(EVENT_OBJECT_URLITEM, pFnd);
        GetDoc()->CallEvent(SvMacroItemId::OnClick, aCallEvent);

        ::LoadURL(*this, sURL, nFilter, sTargetFrameName);
    }
    return bRet;
}

static void LoadURL(SwView& rView, const OUString& rURL, LoadUrlFlags nFilter,
                    const OUString& rTargetFrameName)
{
    SwDocShell* pDShell = rView.GetDocShell();
    OSL_ENSURE( pDShell, "No DocShell?!");
    SfxViewFrame& rViewFrame = rView.GetViewFrame();

    if (!SfxObjectShell::AllowedLinkProtocolFromDocument(rURL, pDShell, rViewFrame.GetFrameWeld()))
        return;

    // We are doing tiledRendering, let the client handles the URL loading,
    // unless we are jumping to a TOC mark.
    if (comphelper::LibreOfficeKit::isActive() && !rURL.startsWith("#"))
    {
        rView.libreOfficeKitViewCallback(LOK_CALLBACK_HYPERLINK_CLICKED, rURL.toUtf8());
        return;
    }

    OUString sTargetFrame(rTargetFrameName);
    if (sTargetFrame.isEmpty() && pDShell)
    {
        using namespace ::com::sun::star;
        uno::Reference<document::XDocumentPropertiesSupplier> xDPS(
            pDShell->GetModel(), uno::UNO_QUERY_THROW);
        uno::Reference<document::XDocumentProperties> xDocProps
            = xDPS->getDocumentProperties();
        sTargetFrame = xDocProps->getDefaultTarget();
    }

    OUString sReferer;
    if( pDShell && pDShell->GetMedium() )
        sReferer = pDShell->GetMedium()->GetName();
    SfxFrameItem aView( SID_DOCFRAME, &rViewFrame );
    SfxStringItem aName( SID_FILE_NAME, rURL );
    SfxStringItem aTargetFrameName( SID_TARGETNAME, sTargetFrame );
    SfxStringItem aReferer( SID_REFERER, sReferer );

    SfxBoolItem aNewView( SID_OPEN_NEW_VIEW, false );
    //#39076# Silent can be removed accordingly to SFX.
    SfxBoolItem aBrowse( SID_BROWSE, true );

    if ((nFilter & LoadUrlFlags::NewView) && !comphelper::LibreOfficeKit::isActive())
        aTargetFrameName.SetValue( "_blank" );

    rViewFrame.GetDispatcher()->ExecuteList(SID_OPENDOC,
            SfxCallMode::ASYNCHRON|SfxCallMode::RECORD,
            {
                &aName,
                &aNewView, /*&aSilent,*/
                &aReferer,
                &aView, &aTargetFrameName,
                &aBrowse
            });
}

void LoadURL( SwViewShell& rVSh, const OUString& rURL, LoadUrlFlags nFilter,
              const OUString& rTargetFrameName )
{
    OSL_ENSURE( !rURL.isEmpty(), "what should be loaded here?" );
    if( rURL.isEmpty() )
        return ;

    // The shell could be 0 also!!!!!
    if (auto pSh = dynamic_cast<SwWrtShell*>(&rVSh))
        ::LoadURL(pSh->GetView(), rURL, nFilter, rTargetFrameName);
}

void SwWrtShell::NavigatorPaste( const NaviContentBookmark& rBkmk,
                                    const sal_uInt16 nAction )
{
    if( EXCHG_IN_ACTION_COPY == nAction )
    {
        // Insert
        OUString sURL = rBkmk.GetURL();
        // Is this is a jump within the current Doc?
        const SwDocShell* pDocShell = GetView().GetDocShell();
        if(pDocShell->HasName())
        {
            const OUString rName = pDocShell->GetMedium()->GetURLObject().GetURLNoMark();

            if (sURL.startsWith(rName))
            {
                if (sURL.getLength()>rName.getLength())
                {
                    sURL = sURL.copy(rName.getLength());
                }
                else
                {
                    sURL.clear();
                }
            }
        }
        SwFormatINetFormat aFormat( sURL, OUString() );
        InsertURL( aFormat, rBkmk.GetDescription() );
    }
    else
    {
        SwSectionData aSection( SectionType::FileLink, GetUniqueSectionName() );
        OUString aLinkFile = o3tl::getToken(rBkmk.GetURL(), 0, '#')
            + OUStringChar(sfx2::cTokenSeparator)
            + OUStringChar(sfx2::cTokenSeparator)
            + o3tl::getToken(rBkmk.GetURL(), 1, '#');
        aSection.SetLinkFileName( aLinkFile );
        aSection.SetProtectFlag( true );
        const SwSection* pIns = InsertSection( aSection );
        if( EXCHG_IN_ACTION_MOVE == nAction && pIns )
        {
            aSection = SwSectionData(*pIns);
            aSection.SetLinkFileName( OUString() );
            aSection.SetType( SectionType::Content );
            aSection.SetProtectFlag( false );

            // the update of content from linked section at time delete
            // the undostack. Then the change of the section don't create
            // any undoobject. -  BUG 69145
            bool bDoesUndo = DoesUndo();
            SwUndoId nLastUndoId(SwUndoId::EMPTY);
            if (GetLastUndoInfo(nullptr, & nLastUndoId))
            {
                if (SwUndoId::INSSECTION != nLastUndoId)
                {
                    DoUndo(false);
                }
            }
            UpdateSection( GetSectionFormatPos( *pIns->GetFormat() ), aSection );
            DoUndo( bDoesUndo );
        }
    }
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
