/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsCOMPtr.h"
#include "nsAtom.h"
#include "nsXBLDocumentInfo.h"
#include "nsIInputStream.h"
#include "nsNameSpaceManager.h"
#include "nsIURI.h"
#include "nsIURL.h"
#include "nsIChannel.h"
#include "nsString.h"
#include "nsReadableUtils.h"
#include "plstr.h"
#include "nsIContent.h"
#include "mozilla/dom/BindContext.h"
#include "mozilla/dom/Document.h"
#include "nsContentUtils.h"
#include "ChildIterator.h"
#ifdef MOZ_XUL
#  include "XULDocument.h"
#endif
#include "nsIXMLContentSink.h"
#include "nsContentCID.h"
#include "mozilla/dom/XMLDocument.h"
#include "jsapi.h"
#include "nsXBLService.h"
#include "nsIXPConnect.h"
#include "nsIScriptContext.h"
#include "nsCRT.h"

// Event listeners
#include "mozilla/EventListenerManager.h"
#include "nsIDOMEventListener.h"
#include "nsAttrName.h"

#include "nsGkAtoms.h"

#include "nsXBLPrototypeHandler.h"

#include "nsXBLPrototypeBinding.h"
#include "nsXBLBinding.h"
#include "nsIPrincipal.h"
#include "nsIScriptSecurityManager.h"
#include "mozilla/dom/XBLChildrenElement.h"

#include "nsNodeUtils.h"
#include "nsJSUtils.h"

#include "mozilla/DeferredFinalize.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/ScriptSettings.h"

using namespace mozilla;
using namespace mozilla::dom;

// Helper classes

/***********************************************************************/
//
// The JS class for XBLBinding
//
static void XBLFinalize(JSFreeOp* fop, JSObject* obj) {
  nsXBLDocumentInfo* docInfo =
      static_cast<nsXBLDocumentInfo*>(::JS_GetPrivate(obj));
  DeferredFinalize(docInfo);
}

static bool XBLEnumerate(JSContext* cx, JS::Handle<JSObject*> obj) {
  nsXBLPrototypeBinding* protoBinding = static_cast<nsXBLPrototypeBinding*>(
      ::JS_GetReservedSlot(obj, 0).toPrivate());
  MOZ_ASSERT(protoBinding);

  return protoBinding->ResolveAllFields(cx, obj);
}

static const JSClassOps gPrototypeJSClassOps = {
    nullptr,     nullptr, XBLEnumerate, nullptr, nullptr, nullptr,
    XBLFinalize, nullptr, nullptr,      nullptr, nullptr};

static const JSClass gPrototypeJSClass = {
    "XBL prototype JSClass",
    JSCLASS_HAS_PRIVATE | JSCLASS_PRIVATE_IS_NSISUPPORTS |
        JSCLASS_FOREGROUND_FINALIZE |
        // Our one reserved slot holds the relevant nsXBLPrototypeBinding
        JSCLASS_HAS_RESERVED_SLOTS(1),
    &gPrototypeJSClassOps};

// Implementation //////////////////////////////////////////////////////////////

// Constructors/Destructors
nsXBLBinding::nsXBLBinding(nsXBLPrototypeBinding* aBinding)
    : mMarkedForDeath(false),
      mPrototypeBinding(aBinding),
      mBoundElement(nullptr) {
  NS_ASSERTION(mPrototypeBinding, "Must have a prototype binding!");
  // Grab a ref to the document info so the prototype binding won't die
  NS_ADDREF(mPrototypeBinding->XBLDocumentInfo());
}

nsXBLBinding::~nsXBLBinding() {
  if (mContent) {
    nsXBLBinding::UnbindAnonymousContent(mContent->OwnerDoc(), mContent);
  }
  nsXBLDocumentInfo* info = mPrototypeBinding->XBLDocumentInfo();
  NS_RELEASE(info);
}

NS_IMPL_CYCLE_COLLECTION_CLASS(nsXBLBinding)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(nsXBLBinding)
  // XXX Probably can't unlink mPrototypeBinding->XBLDocumentInfo(), because
  //     mPrototypeBinding is weak.
  if (tmp->mContent) {
    nsXBLBinding::UnbindAnonymousContent(tmp->mContent->OwnerDoc(),
                                         tmp->mContent);
  }
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mContent)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mNextBinding)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mDefaultInsertionPoint)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mInsertionPoints)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mAnonymousContentList)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(nsXBLBinding)
  NS_CYCLE_COLLECTION_NOTE_EDGE_NAME(cb,
                                     "mPrototypeBinding->XBLDocumentInfo()");
  cb.NoteXPCOMChild(tmp->mPrototypeBinding->XBLDocumentInfo());
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mContent)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mNextBinding)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mDefaultInsertionPoint)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mInsertionPoints)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mAnonymousContentList)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END
NS_IMPL_CYCLE_COLLECTION_ROOT_NATIVE(nsXBLBinding, AddRef)
NS_IMPL_CYCLE_COLLECTION_UNROOT_NATIVE(nsXBLBinding, Release)

void nsXBLBinding::SetBaseBinding(nsXBLBinding* aBinding) {
  if (mNextBinding) {
    NS_ERROR("Base XBL binding is already defined!");
    return;
  }

  mNextBinding = aBinding;  // Comptr handles rel/add
}

nsXBLBinding* nsXBLBinding::GetBindingWithContent() {
  if (mContent) {
    return this;
  }

  return mNextBinding ? mNextBinding->GetBindingWithContent() : nullptr;
}

void nsXBLBinding::BindAnonymousContent(nsIContent* aAnonParent,
                                        nsIContent* aElement) {
  // We need to ensure two things.
  // (1) The anonymous content should be fooled into thinking it's in the bound
  // element's document, assuming that the bound element is in a document
  // Note that we don't change the current doc of aAnonParent here, since that
  // quite simply does not matter.  aAnonParent is just a way of keeping refs
  // to all its kids, which are anonymous content from the point of view of
  // aElement.
  // (2) The children's parent back pointer should not be to this synthetic root
  // but should instead point to the enclosing parent element.
  Document* doc = aElement->GetUncomposedDoc();
  Element* element = aElement->AsElement();

  nsAutoScriptBlocker scriptBlocker;
  BindContext context(*this, *element);
  for (nsIContent* child = aAnonParent->GetFirstChild(); child;
       child = child->GetNextSibling()) {
    child->UnbindFromTree();
    child->SetFlags(NODE_IS_ANONYMOUS_ROOT);
    nsresult rv = child->BindToTree(context, *element);
    if (NS_FAILED(rv)) {
      // Oh, well... Just give up.
      // XXXbz This really shouldn't be a void method!
      child->UnbindFromTree();
      return;
    }

#ifdef MOZ_XUL
    // To make other goodies that happen when an element is added to a XUL
    // document work, we need to notify the XUL document using its special API.
    //
    // FIXME(emilio): Is this needed anymore? Do we really use <linkset> or
    // <link> from XBL stuff?
    if (doc && doc->IsXULDocument()) {
      MOZ_ASSERT(!child->IsXULElement(nsGkAtoms::linkset),
                 "Linkset not allowed in XBL.");
    }
#endif
  }
}

void nsXBLBinding::UnbindAnonymousContent(Document* aDocument,
                                          nsIContent* aAnonParent,
                                          bool aNullParent) {
  nsAutoScriptBlocker scriptBlocker;
  // Hold a strong ref while doing this, just in case.
  nsCOMPtr<nsIContent> anonParent = aAnonParent;
  for (nsIContent* child = aAnonParent->GetFirstChild(); child;
       child = child->GetNextSibling()) {
    child->UnbindFromTree(aNullParent);
  }
}

void nsXBLBinding::SetBoundElement(Element* aElement) {
  mBoundElement = aElement;
  if (mNextBinding) mNextBinding->SetBoundElement(aElement);
}

void nsXBLBinding::GenerateAnonymousContent() {
  NS_ASSERTION(!nsContentUtils::IsSafeToRunScript(),
               "Someone forgot a script blocker");

  // Fetch the content element for this binding.
  Element* content = mPrototypeBinding->GetImmediateChild(nsGkAtoms::content);

  if (!content) {
    // We have no anonymous content.
    if (mNextBinding) mNextBinding->GenerateAnonymousContent();

    return;
  }

  // Find out if we're really building kids or if we're just
  // using the attribute-setting shorthand hack.
  uint32_t contentCount = content->GetChildCount();

  // Plan to build the content by default.
  bool hasContent = (contentCount > 0);
  if (hasContent) {
    Document* doc = mBoundElement->OwnerDoc();

    nsCOMPtr<nsINode> clonedNode = nsNodeUtils::Clone(
        content, true, doc->NodeInfoManager(), nullptr, IgnoreErrors());
    // FIXME: Bug 1399558, Why is this code OK assuming that nsNodeUtils::Clone
    // never fails?
    mContent = clonedNode->AsElement();

    // Search for <xbl:children> elements in the XBL content. In the presence
    // of multiple default insertion points, we use the last one in document
    // order.
    for (nsIContent* child = mContent; child;
         child = child->GetNextNode(mContent)) {
      if (child->NodeInfo()->Equals(nsGkAtoms::children, kNameSpaceID_XBL)) {
        XBLChildrenElement* point = static_cast<XBLChildrenElement*>(child);
        if (point->IsDefaultInsertion()) {
          mDefaultInsertionPoint = point;
        } else {
          mInsertionPoints.AppendElement(point);
        }
      }
    }

    // Do this after looking for <children> as this messes up the parent
    // pointer which would make the GetNextNode call above fail
    BindAnonymousContent(mContent, mBoundElement);

    // Insert explicit children into insertion points
    if (mDefaultInsertionPoint && mInsertionPoints.IsEmpty()) {
      ExplicitChildIterator iter(mBoundElement);
      for (nsIContent* child = iter.GetNextChild(); child;
           child = iter.GetNextChild()) {
        // Pass aNotify = false because we're just setting up the whole thing.
        // Furthermore we do it from frame construction, so passing true here
        // would reenter into it which is... not great.
        mDefaultInsertionPoint->AppendInsertedChild(child, false);
      }
    } else {
      // It is odd to come into this code if mInsertionPoints is not empty, but
      // we need to make sure to do the compatibility hack below if the bound
      // node has any non <xul:template> or <xul:observes> children.
      ExplicitChildIterator iter(mBoundElement);
      for (nsIContent* child = iter.GetNextChild(); child;
           child = iter.GetNextChild()) {
        XBLChildrenElement* point = FindInsertionPointForInternal(child);
        if (point) {
          // Pass aNotify = false because we're just setting up the whole thing.
          // (see the similar call above for more details).
          point->AppendInsertedChild(child, false);
        } else {
          NodeInfo* ni = child->NodeInfo();
          if (!child->TextIsOnlyWhitespace() &&
              (ni->NamespaceID() != kNameSpaceID_XUL ||
               (!ni->Equals(nsGkAtoms::_template) &&
                !ni->Equals(nsGkAtoms::observes)))) {
            // Compatibility hack. For some reason the original XBL
            // implementation dropped the content of a binding if any child of
            // the bound element didn't match any of the <children> in the
            // binding. This became a pseudo-API that we have to maintain.

            // Undo BindAnonymousContent
            UnbindAnonymousContent(doc, mContent);

            // Clear out our children elements to avoid dangling references.
            ClearInsertionPoints();

            // Pretend as though there was no content in the binding.
            mContent = nullptr;
            return;
          }
        }
      }
    }

    // Set binding parent on default content if need
    if (mDefaultInsertionPoint) {
      mDefaultInsertionPoint->MaybeSetupDefaultContent();
    }
    for (uint32_t i = 0; i < mInsertionPoints.Length(); ++i) {
      mInsertionPoints[i]->MaybeSetupDefaultContent();
    }

    mPrototypeBinding->SetInitialAttributes(mBoundElement, mContent);
  }

  // Always check the content element for potential attributes.
  // This shorthand hack always happens, even when we didn't
  // build anonymous content.
  BorrowedAttrInfo attrInfo;
  for (uint32_t i = 0; (attrInfo = content->GetAttrInfoAt(i)); ++i) {
    int32_t namespaceID = attrInfo.mName->NamespaceID();
    // Hold a strong reference here so that the atom doesn't go away during
    // UnsetAttr.
    RefPtr<nsAtom> name = attrInfo.mName->LocalName();

    if (name != nsGkAtoms::includes) {
      if (!nsContentUtils::HasNonEmptyAttr(mBoundElement, namespaceID, name)) {
        nsAutoString value2;
        attrInfo.mValue->ToString(value2);
        mBoundElement->SetAttr(namespaceID, name, attrInfo.mName->GetPrefix(),
                               value2, false);
      }
    }

    // Conserve space by wiping the attributes off the clone.
    //
    // FIXME(emilio): It'd be nice to make `mContent` a `RefPtr<Element>`.
    if (mContent) mContent->AsElement()->UnsetAttr(namespaceID, name, false);
  }
}

nsIURI* nsXBLBinding::GetSourceDocURI() {
  nsIContent* targetContent =
      mPrototypeBinding->GetImmediateChild(nsGkAtoms::content);
  if (!targetContent) {
    return nullptr;
  }

  return targetContent->OwnerDoc()->GetDocumentURI();
}

XBLChildrenElement* nsXBLBinding::FindInsertionPointFor(nsIContent* aChild) {
  // XXX We should get rid of this function as it causes us to traverse the
  // binding chain multiple times
  if (mContent) {
    return FindInsertionPointForInternal(aChild);
  }

  return mNextBinding ? mNextBinding->FindInsertionPointFor(aChild) : nullptr;
}

XBLChildrenElement* nsXBLBinding::FindInsertionPointForInternal(
    nsIContent* aChild) {
  for (uint32_t i = 0; i < mInsertionPoints.Length(); ++i) {
    XBLChildrenElement* point = mInsertionPoints[i];
    if (point->Includes(aChild)) {
      return point;
    }
  }

  return mDefaultInsertionPoint;
}

void nsXBLBinding::ClearInsertionPoints() {
  if (mDefaultInsertionPoint) {
    mDefaultInsertionPoint->ClearInsertedChildren();
  }

  for (const auto& insertionPoint : mInsertionPoints) {
    insertionPoint->ClearInsertedChildren();
  }
}

nsAnonymousContentList* nsXBLBinding::GetAnonymousNodeList() {
  if (!mContent) {
    return mNextBinding ? mNextBinding->GetAnonymousNodeList() : nullptr;
  }

  if (!mAnonymousContentList) {
    mAnonymousContentList = new nsAnonymousContentList(mContent);
  }

  return mAnonymousContentList;
}

void nsXBLBinding::InstallEventHandlers() {
  // Don't install handlers if scripts aren't allowed.
  if (AllowScripts()) {
    // Fetch the handlers prototypes for this binding.
    nsXBLPrototypeHandler* handlerChain =
        mPrototypeBinding->GetPrototypeHandlers();

    if (handlerChain) {
      EventListenerManager* manager =
          mBoundElement->GetOrCreateListenerManager();
      if (!manager) return;

      bool isChromeDoc = nsContentUtils::IsChromeDoc(mBoundElement->OwnerDoc());
      bool isChromeBinding = mPrototypeBinding->IsChrome();
      nsXBLPrototypeHandler* curr;
      for (curr = handlerChain; curr; curr = curr->GetNextHandler()) {
        // Fetch the event type.
        RefPtr<nsAtom> eventAtom = curr->GetEventName();
        if (!eventAtom || eventAtom == nsGkAtoms::keyup ||
            eventAtom == nsGkAtoms::keydown || eventAtom == nsGkAtoms::keypress)
          continue;

        nsXBLEventHandler* handler = curr->GetEventHandler();
        if (handler) {
          // Figure out if we're using capturing or not.
          EventListenerFlags flags;
          flags.mCapture = (curr->GetPhase() == NS_PHASE_CAPTURING);

          // If this is a command, add it in the system event group
          if ((curr->GetType() &
               (NS_HANDLER_TYPE_XBL_COMMAND | NS_HANDLER_TYPE_SYSTEM)) &&
              (isChromeBinding ||
               mBoundElement->IsInNativeAnonymousSubtree())) {
            flags.mInSystemGroup = true;
          }

          bool hasAllowUntrustedAttr = curr->HasAllowUntrustedAttr();
          if ((hasAllowUntrustedAttr && curr->AllowUntrustedEvents()) ||
              (!hasAllowUntrustedAttr && !isChromeDoc)) {
            flags.mAllowUntrustedEvents = true;
          }

          manager->AddEventListenerByType(
              handler, nsDependentAtomString(eventAtom), flags);
        }
      }

      const nsCOMArray<nsXBLKeyEventHandler>* keyHandlers =
          mPrototypeBinding->GetKeyEventHandlers();
      int32_t i;
      for (i = 0; i < keyHandlers->Count(); ++i) {
        nsXBLKeyEventHandler* handler = keyHandlers->ObjectAt(i);
        handler->SetIsBoundToChrome(isChromeDoc);

        nsAutoString type;
        handler->GetEventName(type);

        // If this is a command, add it in the system event group, otherwise
        // add it to the standard event group.

        // Figure out if we're using capturing or not.
        EventListenerFlags flags;
        flags.mCapture = (handler->GetPhase() == NS_PHASE_CAPTURING);

        if ((handler->GetType() &
             (NS_HANDLER_TYPE_XBL_COMMAND | NS_HANDLER_TYPE_SYSTEM)) &&
            (isChromeBinding || mBoundElement->IsInNativeAnonymousSubtree())) {
          flags.mInSystemGroup = true;
        }

        // For key handlers we have to set mAllowUntrustedEvents flag.
        // Whether the handling of the event is allowed or not is handled in
        // nsXBLKeyEventHandler::HandleEvent
        flags.mAllowUntrustedEvents = true;

        manager->AddEventListenerByType(handler, type, flags);
      }
    }
  }

  if (mNextBinding) mNextBinding->InstallEventHandlers();
}

nsresult nsXBLBinding::InstallImplementation() {
  // Always install the base class properties first, so that
  // derived classes can reference the base class properties.

  if (mNextBinding) {
    nsresult rv = mNextBinding->InstallImplementation();
    NS_ENSURE_SUCCESS(rv, rv);
  }

  // iterate through each property in the prototype's list and install the
  // property.
  if (AllowScripts()) return mPrototypeBinding->InstallImplementation(this);

  return NS_OK;
}

void nsXBLBinding::AttributeChanged(nsAtom* aAttribute, int32_t aNameSpaceID,
                                    bool aRemoveFlag, bool aNotify) {
  // XXX Change if we ever allow multiple bindings in a chain to contribute
  // anonymous content
  if (!mContent) {
    if (mNextBinding)
      mNextBinding->AttributeChanged(aAttribute, aNameSpaceID, aRemoveFlag,
                                     aNotify);
  } else {
    mPrototypeBinding->AttributeChanged(aAttribute, aNameSpaceID, aRemoveFlag,
                                        mBoundElement, mContent, aNotify);
  }
}

void nsXBLBinding::ExecuteAttachedHandler() {
  if (mNextBinding) mNextBinding->ExecuteAttachedHandler();

  if (AllowScripts()) mPrototypeBinding->BindingAttached(mBoundElement);
}

void nsXBLBinding::ExecuteDetachedHandler() {
  if (AllowScripts()) mPrototypeBinding->BindingDetached(mBoundElement);

  if (mNextBinding) mNextBinding->ExecuteDetachedHandler();
}

void nsXBLBinding::UnhookEventHandlers() {
  nsXBLPrototypeHandler* handlerChain =
      mPrototypeBinding->GetPrototypeHandlers();

  if (handlerChain) {
    EventListenerManager* manager = mBoundElement->GetExistingListenerManager();
    if (!manager) {
      return;
    }

    bool isChromeBinding = mPrototypeBinding->IsChrome();
    nsXBLPrototypeHandler* curr;
    for (curr = handlerChain; curr; curr = curr->GetNextHandler()) {
      nsXBLEventHandler* handler = curr->GetCachedEventHandler();
      if (!handler) {
        continue;
      }

      RefPtr<nsAtom> eventAtom = curr->GetEventName();
      if (!eventAtom || eventAtom == nsGkAtoms::keyup ||
          eventAtom == nsGkAtoms::keydown || eventAtom == nsGkAtoms::keypress)
        continue;

      // Figure out if we're using capturing or not.
      EventListenerFlags flags;
      flags.mCapture = (curr->GetPhase() == NS_PHASE_CAPTURING);

      // If this is a command, remove it from the system event group,
      // otherwise remove it from the standard event group.

      if ((curr->GetType() &
           (NS_HANDLER_TYPE_XBL_COMMAND | NS_HANDLER_TYPE_SYSTEM)) &&
          (isChromeBinding || mBoundElement->IsInNativeAnonymousSubtree())) {
        flags.mInSystemGroup = true;
      }

      manager->RemoveEventListenerByType(
          handler, nsDependentAtomString(eventAtom), flags);
    }

    const nsCOMArray<nsXBLKeyEventHandler>* keyHandlers =
        mPrototypeBinding->GetKeyEventHandlers();
    int32_t i;
    for (i = 0; i < keyHandlers->Count(); ++i) {
      nsXBLKeyEventHandler* handler = keyHandlers->ObjectAt(i);

      nsAutoString type;
      handler->GetEventName(type);

      // Figure out if we're using capturing or not.
      EventListenerFlags flags;
      flags.mCapture = (handler->GetPhase() == NS_PHASE_CAPTURING);

      // If this is a command, remove it from the system event group, otherwise
      // remove it from the standard event group.

      if ((handler->GetType() &
           (NS_HANDLER_TYPE_XBL_COMMAND | NS_HANDLER_TYPE_SYSTEM)) &&
          (isChromeBinding || mBoundElement->IsInNativeAnonymousSubtree())) {
        flags.mInSystemGroup = true;
      }

      manager->RemoveEventListenerByType(handler, type, flags);
    }
  }
}

void nsXBLBinding::ChangeDocument(Document* aOldDocument,
                                  Document* aNewDocument) {
  if (aOldDocument == aNewDocument) return;

  // Now the binding dies.  Unhook our prototypes.
  if (mPrototypeBinding->HasImplementation()) {
    AutoJSAPI jsapi;
    // Init might fail here if we've cycle-collected the global object, since
    // the Unlink phase of cycle collection happens after JS GC finalization.
    // But in that case, we don't care about fixing the prototype chain, since
    // everything's going away immediately.
    if (jsapi.Init(aOldDocument->GetScopeObject())) {
      JSContext* cx = jsapi.cx();

      JS::Rooted<JSObject*> scriptObject(cx, mBoundElement->GetWrapper());
      if (scriptObject) {
        // XXX Stay in sync! What if a layered binding has an
        // <interface>?!
        // XXXbz what does that comment mean, really?  It seems to date
        // back to when there was such a thing as an <interface>, whever
        // that was...

        // Find the right prototype.
        JSAutoRealm ar(cx, scriptObject);

        JS::Rooted<JSObject*> base(cx, scriptObject);
        JS::Rooted<JSObject*> proto(cx);
        for (; true; base = proto) {  // Will break out on null proto
          if (!JS_GetPrototype(cx, base, &proto)) {
            return;
          }
          if (!proto) {
            break;
          }

          if (JS_GetClass(proto) != &gPrototypeJSClass) {
            // Clearly not the right class
            continue;
          }

          RefPtr<nsXBLDocumentInfo> docInfo =
              static_cast<nsXBLDocumentInfo*>(::JS_GetPrivate(proto));
          if (!docInfo) {
            // Not the proto we seek
            continue;
          }

          JS::Value protoBinding = ::JS_GetReservedSlot(proto, 0);

          if (protoBinding.toPrivate() != mPrototypeBinding) {
            // Not the right binding
            continue;
          }

          // Alright!  This is the right prototype.  Pull it out of the
          // proto chain.
          JS::Rooted<JSObject*> grandProto(cx);
          if (!JS_GetPrototype(cx, proto, &grandProto)) {
            return;
          }
          ::JS_SetPrototype(cx, base, grandProto);
          break;
        }

        mPrototypeBinding->UndefineFields(cx, scriptObject);

        // Don't remove the reference from the document to the
        // wrapper here since it'll be removed by the element
        // itself when that's taken out of the document.
      }
    }
  }

  // Remove our event handlers
  UnhookEventHandlers();

  {
    nsAutoScriptBlocker scriptBlocker;

    // Then do our ancestors.  This reverses the construction order, so that at
    // all times things are consistent as far as everyone is concerned.
    if (mNextBinding) {
      mNextBinding->ChangeDocument(aOldDocument, aNewDocument);
    }

    // Update the anonymous content.
    // XXXbz why not only for style bindings?
    if (mContent) {
      nsXBLBinding::UnbindAnonymousContent(aOldDocument, mContent);
    }

    ClearInsertionPoints();
  }
}

// Internal helper methods /////////////////////////////////////////////////////

// Get or create a WeakMap object on a given XBL-hosting global.
//
// The scheme is as follows. XBL-hosting globals (either privileged content
// Windows or XBL scopes) get two lazily-defined WeakMap properties. Each
// WeakMap is keyed by the grand-proto - i.e. the original prototype of the
// content before it was bound, and the prototype of the class object that we
// splice in. The values in the WeakMap are simple dictionary-style objects,
// mapping from XBL class names to class objects.
static JSObject* GetOrCreateClassObjectMap(JSContext* cx,
                                           JS::Handle<JSObject*> scope,
                                           const char* mapName) {
  AssertSameCompartment(cx, scope);
  MOZ_ASSERT(JS_IsGlobalObject(scope));
  MOZ_ASSERT(scope == xpc::GetXBLScopeOrGlobal(cx, scope));

  // First, see if the map is already defined.
  JS::Rooted<JS::PropertyDescriptor> desc(cx);
  if (!JS_GetOwnPropertyDescriptor(cx, scope, mapName, &desc)) {
    return nullptr;
  }
  if (desc.object() && desc.value().isObject() &&
      JS::IsWeakMapObject(&desc.value().toObject())) {
    return &desc.value().toObject();
  }

  // It's not there. Create and define it.
  JS::Rooted<JSObject*> map(cx, JS::NewWeakMapObject(cx));
  if (!map || !JS_DefineProperty(cx, scope, mapName, map,
                                 JSPROP_PERMANENT | JSPROP_READONLY)) {
    return nullptr;
  }
  return map;
}

static JSObject* GetOrCreateMapEntryForPrototype(JSContext* cx,
                                                 JS::Handle<JSObject*> proto) {
  AssertSameCompartment(cx, proto);
  // We want to hang our class objects off the XBL scope. But since we also
  // hoist anonymous content into the XBL scope, this creates the potential for
  // tricky collisions, since we can simultaneously  have a bound in-content
  // node with grand-proto HTMLDivElement and a bound anonymous node whose
  // grand-proto is the XBL scope's cross-compartment wrapper to HTMLDivElement.
  // Since we have to wrap the WeakMap keys into its scope, this distinction
  // would be lost if we don't do something about it.
  //
  // So we define two maps - one class objects that live in content (prototyped
  // to content prototypes), and the other for class objects that live in the
  // XBL scope (prototyped to cross-compartment-wrapped content prototypes).
  const char* name = xpc::IsInContentXBLScope(proto)
                         ? "__ContentClassObjectMap__"
                         : "__XBLClassObjectMap__";

  // Now, enter the XBL scope, since that's where we need to operate, and wrap
  // the proto accordingly. We hang the map off of the content XBL scope for
  // content, and the Window for chrome (whether add-ons are involved or not).
  JS::Rooted<JSObject*> scope(
      cx, xpc::GetXBLScopeOrGlobal(cx, JS::CurrentGlobalOrNull(cx)));
  NS_ENSURE_TRUE(scope, nullptr);
  MOZ_ASSERT(JS_IsGlobalObject(scope));

  JS::Rooted<JSObject*> wrappedProto(cx, proto);
  JSAutoRealm ar(cx, scope);
  if (!JS_WrapObject(cx, &wrappedProto)) {
    return nullptr;
  }

  // Grab the appropriate WeakMap.
  JS::Rooted<JSObject*> map(cx, GetOrCreateClassObjectMap(cx, scope, name));
  if (!map) {
    return nullptr;
  }

  // See if we already have a map entry for that prototype.
  JS::Rooted<JS::Value> val(cx);
  if (!JS::GetWeakMapEntry(cx, map, wrappedProto, &val)) {
    return nullptr;
  }
  if (val.isObject()) {
    return &val.toObject();
  }

  // We don't have an entry. Create one and stick it in the map.
  JS::Rooted<JSObject*> entry(cx);
  entry = JS_NewObjectWithGivenProto(cx, nullptr, nullptr);
  if (!entry) {
    return nullptr;
  }
  JS::Rooted<JS::Value> entryVal(cx, JS::ObjectValue(*entry));
  if (!JS::SetWeakMapEntry(cx, map, wrappedProto, entryVal)) {
    NS_WARNING(
        "SetWeakMapEntry failed, probably due to non-preservable WeakMap "
        "key. XBL binding will fail for this element.");
    return nullptr;
  }
  return entry;
}

static nsXBLPrototypeBinding* GetProtoBindingFromClassObject(JSObject* obj) {
  MOZ_ASSERT(JS_GetClass(obj) == &gPrototypeJSClass);
  return static_cast<nsXBLPrototypeBinding*>(
      ::JS_GetReservedSlot(obj, 0).toPrivate());
}

// static
nsresult nsXBLBinding::DoInitJSClass(JSContext* cx, JS::Handle<JSObject*> obj,
                                     const nsString& aClassName,
                                     nsXBLPrototypeBinding* aProtoBinding,
                                     JS::MutableHandle<JSObject*> aClassObject,
                                     bool* aNew) {
  MOZ_ASSERT(obj);

  // Note that, now that NAC reflectors are created in the XBL scope, the
  // reflector is not necessarily same-compartment with the document. So we'll
  // end up creating a separate instance of the oddly-named XBL class object
  // and defining it as a property on the XBL scope's global. This works fine,
  // but we need to make sure never to assume that the the reflector and
  // prototype are same-compartment with the bound document.
  JS::Rooted<JSObject*> global(cx, JS::GetNonCCWObjectGlobal(obj));

  // We must be in obj's realm.
  MOZ_ASSERT(JS::CurrentGlobalOrNull(cx) == global);

  // We never store class objects in add-on scopes.
  JS::Rooted<JSObject*> xblScope(cx, xpc::GetXBLScopeOrGlobal(cx, global));
  NS_ENSURE_TRUE(xblScope, NS_ERROR_UNEXPECTED);

  JS::Rooted<JSObject*> parent_proto(cx);
  {
    JS::RootedObject wrapped(cx, obj);
    JSAutoRealm ar(cx, xblScope);
    if (!JS_WrapObject(cx, &wrapped)) {
      return NS_ERROR_FAILURE;
    }
    if (!JS_GetPrototype(cx, wrapped, &parent_proto)) {
      return NS_ERROR_FAILURE;
    }
  }
  if (!JS_WrapObject(cx, &parent_proto)) {
    return NS_ERROR_FAILURE;
  }

  // Get the map entry for the parent prototype. In the one-off case that the
  // parent prototype is null, we somewhat hackily just use the WeakMap itself
  // as a property holder.
  JS::Rooted<JSObject*> holder(cx);
  if (parent_proto) {
    holder = GetOrCreateMapEntryForPrototype(cx, parent_proto);
  } else {
    JSAutoRealm innerAR(cx, xblScope);
    holder =
        GetOrCreateClassObjectMap(cx, xblScope, "__ContentClassObjectMap__");
  }
  if (NS_WARN_IF(!holder)) {
    return NS_ERROR_FAILURE;
  }
  js::AssertSameCompartment(holder, xblScope);
  JSAutoRealm ar(cx, holder);

  // Look up the class on the property holder. The only properties on the
  // holder should be class objects. If we don't find the class object, we need
  // to create and define it.
  JS::Rooted<JSObject*> proto(cx);
  JS::Rooted<JS::PropertyDescriptor> desc(cx);
  if (!JS_GetOwnUCPropertyDescriptor(cx, holder, aClassName.get(),
                                     aClassName.Length(), &desc)) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  *aNew = !desc.object();
  if (desc.object()) {
    proto = &desc.value().toObject();
    DebugOnly<nsXBLPrototypeBinding*> cachedBinding =
        GetProtoBindingFromClassObject(js::UncheckedUnwrap(proto));
    MOZ_ASSERT(cachedBinding == aProtoBinding);
  } else {
    // We need to create the prototype. First, enter the realm where it's
    // going to live, and create it.
    JSAutoRealm ar2(cx, global);
    proto = JS_NewObjectWithGivenProto(cx, &gPrototypeJSClass, parent_proto);
    if (!proto) {
      return NS_ERROR_OUT_OF_MEMORY;
    }

    // Keep this proto binding alive while we're alive.  Do this first so that
    // we can guarantee that in XBLFinalize this will be non-null.
    // Note that we can't just store aProtoBinding in the private and
    // addref/release the nsXBLDocumentInfo through it, because cycle
    // collection doesn't seem to work right if the private is not an
    // nsISupports.
    nsXBLDocumentInfo* docInfo = aProtoBinding->XBLDocumentInfo();
    ::JS_SetPrivate(proto, docInfo);
    NS_ADDREF(docInfo);
    RecordReplayRegisterDeferredFinalize(docInfo);
    JS_SetReservedSlot(proto, 0, JS::PrivateValue(aProtoBinding));

    // Next, enter the realm of the property holder, wrap the proto, and
    // stick it on.
    JSAutoRealm ar3(cx, holder);
    if (!JS_WrapObject(cx, &proto) ||
        !JS_DefineUCProperty(cx, holder, aClassName.get(), -1, proto,
                             JSPROP_READONLY | JSPROP_PERMANENT)) {
      return NS_ERROR_OUT_OF_MEMORY;
    }
  }

  // Whew. We have the proto. Wrap it back into the realm of |obj|,
  // splice it in, and return it.
  JSAutoRealm ar4(cx, obj);
  if (!JS_WrapObject(cx, &proto) || !JS_SetPrototype(cx, obj, proto)) {
    return NS_ERROR_FAILURE;
  }
  aClassObject.set(proto);
  return NS_OK;
}

bool nsXBLBinding::AllowScripts() {
  return mBoundElement && mPrototypeBinding->GetAllowScripts();
}

nsXBLBinding* nsXBLBinding::RootBinding() {
  if (mNextBinding) return mNextBinding->RootBinding();

  return this;
}

bool nsXBLBinding::ResolveAllFields(JSContext* cx,
                                    JS::Handle<JSObject*> obj) const {
  if (!mPrototypeBinding->ResolveAllFields(cx, obj)) {
    return false;
  }

  if (mNextBinding) {
    return mNextBinding->ResolveAllFields(cx, obj);
  }

  return true;
}

bool nsXBLBinding::LookupMember(
    JSContext* aCx, JS::Handle<jsid> aId,
    JS::MutableHandle<JS::PropertyDescriptor> aDesc) {
  // We should never enter this function with a pre-filled property descriptor.
  MOZ_ASSERT(!aDesc.object());

  // Get the string as an nsString before doing anything, so we can make
  // convenient comparisons during our search.
  if (!JSID_IS_STRING(aId)) {
    return true;
  }
  nsAutoJSString name;
  if (!name.init(aCx, JSID_TO_STRING(aId))) {
    return false;
  }

  // We have a weak reference to our bound element, so make sure it's alive.
  if (!mBoundElement || !mBoundElement->GetWrapper()) {
    return false;
  }

  // Get the scope of mBoundElement and the associated XBL scope. We should only
  // be calling into this machinery if we're running in a separate XBL scope.
  //
  // Note that we only end up in LookupMember for XrayWrappers from XBL scopes
  // into content. So for NAC reflectors that live in the XBL scope, we should
  // never get here. But on the off-chance that someone adds new callsites to
  // LookupMember, we do a release-mode assertion as belt-and-braces.
  // We do a release-mode assertion here to be extra safe.
  //
  // This code is only called for content XBL, so we don't have to worry about
  // add-on scopes here.
  JS::Rooted<JSObject*> boundScope(
      aCx, JS::GetNonCCWObjectGlobal(mBoundElement->GetWrapper()));
  MOZ_RELEASE_ASSERT(!xpc::IsInContentXBLScope(boundScope));
  JS::Rooted<JSObject*> xblScope(aCx, xpc::GetXBLScope(aCx, boundScope));
  NS_ENSURE_TRUE(xblScope, false);
  MOZ_ASSERT(boundScope != xblScope);

  // Enter the xbl scope and invoke the internal version.
  {
    JSAutoRealm ar(aCx, xblScope);
    JS::Rooted<jsid> id(aCx, aId);
    if (!LookupMemberInternal(aCx, name, id, aDesc, xblScope)) {
      return false;
    }
  }

  // Wrap into the caller's scope.
  return JS_WrapPropertyDescriptor(aCx, aDesc);
}

bool nsXBLBinding::LookupMemberInternal(
    JSContext* aCx, nsString& aName, JS::Handle<jsid> aNameAsId,
    JS::MutableHandle<JS::PropertyDescriptor> aDesc,
    JS::Handle<JSObject*> aXBLScope) {
  // First, see if we have an implementation. If we don't, it means that this
  // binding doesn't have a class object, and thus doesn't have any members.
  // Skip it.
  if (!PrototypeBinding()->HasImplementation()) {
    if (!mNextBinding) {
      return true;
    }
    return mNextBinding->LookupMemberInternal(aCx, aName, aNameAsId, aDesc,
                                              aXBLScope);
  }

  // Find our class object. It's in a protected scope and permanent just in
  // case, so should be there no matter what.
  JS::Rooted<JS::Value> classObject(aCx);
  if (!JS_GetUCProperty(aCx, aXBLScope, PrototypeBinding()->ClassName().get(),
                        -1, &classObject)) {
    return false;
  }

  // The bound element may have been adoped by a document and have a different
  // wrapper (and different xbl scope) than when the binding was applied, in
  // this case getting the class object will fail. Behave as if the class
  // object did not exist.
  if (classObject.isUndefined()) {
    return true;
  }

  MOZ_ASSERT(classObject.isObject());

  // Look for the property on this binding. If it's not there, try the next
  // binding on the chain.
  nsXBLProtoImpl* impl = mPrototypeBinding->GetImplementation();
  JS::Rooted<JSObject*> object(aCx, &classObject.toObject());
  if (impl && !impl->LookupMember(aCx, aName, aNameAsId, aDesc, object)) {
    return false;
  }
  if (aDesc.object() || !mNextBinding) {
    return true;
  }

  return mNextBinding->LookupMemberInternal(aCx, aName, aNameAsId, aDesc,
                                            aXBLScope);
}

bool nsXBLBinding::HasField(nsString& aName) {
  // See if this binding has such a field.
  return mPrototypeBinding->FindField(aName) ||
         (mNextBinding && mNextBinding->HasField(aName));
}

void nsXBLBinding::MarkForDeath() {
  mMarkedForDeath = true;
  ExecuteDetachedHandler();
}

bool nsXBLBinding::ImplementsInterface(REFNSIID aIID) const {
  return mPrototypeBinding->ImplementsInterface(aIID) ||
         (mNextBinding && mNextBinding->ImplementsInterface(aIID));
}
