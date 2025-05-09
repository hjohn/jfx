/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 *           (C) 2001 Dirk Mueller (mueller@kde.org)
 * Copyright (C) 2003-2017 Apple Inc. All rights reserved.
 * Copyright (C) 2009 Rob Buis (rwlbuis@gmail.com)
 * Copyright (C) 2011 Google Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "HTMLLinkElement.h"

#include "Attribute.h"
#include "CachedCSSStyleSheet.h"
#include "CachedResource.h"
#include "CachedResourceLoader.h"
#include "CachedResourceRequest.h"
#include "ContentSecurityPolicy.h"
#include "CrossOriginAccessControl.h"
#include "DOMTokenList.h"
#include "DefaultResourceLoadPriority.h"
#include "DocumentInlines.h"
#include "DocumentLoader.h"
#include "ElementInlines.h"
#include "Event.h"
#include "EventNames.h"
#include "EventSender.h"
#include "FrameLoader.h"
#include "FrameTree.h"
#include "HTMLAnchorElement.h"
#include "HTMLNames.h"
#include "HTMLParserIdioms.h"
#include "IdTargetObserver.h"
#include "JSRequestPriority.h"
#include "LocalFrame.h"
#include "LocalFrameLoaderClient.h"
#include "LocalFrameView.h"
#include "Logging.h"
#include "MediaQueryEvaluator.h"
#include "MediaQueryParser.h"
#include "MediaQueryParserContext.h"
#include "MouseEvent.h"
#include "NodeName.h"
#include "Page.h"
#include "ParsedContentType.h"
#include "RenderStyle.h"
#include "RequestPriority.h"
#include "SecurityOrigin.h"
#include "Settings.h"
#include "StyleInheritedData.h"
#include "StyleResolveForDocument.h"
#include "StyleScope.h"
#include "StyleSheetContents.h"
#include "SubresourceIntegrity.h"
#include "TreeScopeInlines.h"
#include <wtf/Ref.h>
#include <wtf/Scope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/TextStream.h>

namespace WebCore {

WTF_MAKE_TZONE_OR_ISO_ALLOCATED_IMPL(HTMLLinkElement);

using namespace HTMLNames;

static LinkEventSender& linkLoadEventSender()
{
    static NeverDestroyed<LinkEventSender> sharedLoadEventSender;
    return sharedLoadEventSender;
}

class ExpectIdTargetObserver final : public IdTargetObserver {
    WTF_MAKE_FAST_ALLOCATED;
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(ExpectIdTargetObserver);
public:
    ExpectIdTargetObserver(const AtomString& id, HTMLLinkElement&);

    void idTargetChanged() override;

private:
    WeakPtr<HTMLLinkElement, WeakPtrImplWithEventTargetData> m_element;
};

ExpectIdTargetObserver::ExpectIdTargetObserver(const AtomString& id, HTMLLinkElement& element)
    : IdTargetObserver(element.treeScope().idTargetObserverRegistry(), id)
    , m_element(element)
{
}

void ExpectIdTargetObserver::idTargetChanged()
{
    if (m_element)
        m_element->processInternalResourceLink();
}

inline HTMLLinkElement::HTMLLinkElement(const QualifiedName& tagName, Document& document, bool createdByParser)
    : HTMLElement(tagName, document)
    , m_linkLoader(*this)
    , m_disabledState(Unset)
    , m_loading(false)
    , m_createdByParser(createdByParser)
    , m_loadedResource(false)
    , m_isHandlingBeforeLoad(false)
    , m_allowPrefetchLoadAndErrorForTesting(false)
    , m_pendingSheetType(Unknown)
{
    ASSERT(hasTagName(linkTag));
}

Ref<HTMLLinkElement> HTMLLinkElement::create(const QualifiedName& tagName, Document& document, bool createdByParser)
{
    return adoptRef(*new HTMLLinkElement(tagName, document, createdByParser));
}

HTMLLinkElement::~HTMLLinkElement()
{
    if (m_sheet)
        m_sheet->clearOwnerNode();

    if (m_cachedSheet)
        m_cachedSheet->removeClient(*this);

    if (m_styleScope)
        m_styleScope->removeStyleSheetCandidateNode(*this);

    linkLoadEventSender().cancelEvent(*this);
}

void HTMLLinkElement::setDisabledState(bool disabled)
{
    DisabledState oldDisabledState = m_disabledState;
    m_disabledState = disabled ? Disabled : EnabledViaScript;
    if (oldDisabledState == m_disabledState)
        return;

    ASSERT(isConnected() || !styleSheetIsLoading());
    if (!isConnected())
        return;

    // If we change the disabled state while the sheet is still loading, then we have to
    // perform three checks:
    if (styleSheetIsLoading()) {
        // Check #1: The sheet becomes disabled while loading.
        if (m_disabledState == Disabled)
            removePendingSheet();

        // Check #2: An alternate sheet becomes enabled while it is still loading.
        if (m_relAttribute.isAlternate && m_disabledState == EnabledViaScript)
            addPendingSheet(ActiveSheet);

        // Check #3: A main sheet becomes enabled while it was still loading and
        // after it was disabled via script. It takes really terrible code to make this
        // happen (a double toggle for no reason essentially). This happens on
        // virtualplastic.net, which manages to do about 12 enable/disables on only 3
        // sheets. :)
        if (!m_relAttribute.isAlternate && m_disabledState == EnabledViaScript && oldDisabledState == Disabled)
            addPendingSheet(ActiveSheet);

        // If the sheet is already loading just bail.
        return;
    }

    // Load the sheet, since it's never been loaded before.
    if (!m_sheet && m_disabledState == EnabledViaScript)
        process();
    else {
        ASSERT(m_styleScope);
        m_styleScope->didChangeActiveStyleSheetCandidates();
        if (m_sheet)
            clearSheet();
    }
}

void HTMLLinkElement::attributeChanged(const QualifiedName& name, const AtomString& oldValue, const AtomString& newValue, AttributeModificationReason attributeModificationReason)
{
    switch (name.nodeName()) {
    case AttributeNames::relAttr: {
        auto parsedRel = LinkRelAttribute(document(), newValue);
        auto didMutateRel = parsedRel != m_relAttribute;
        m_relAttribute = WTFMove(parsedRel);
        if (m_relList)
            m_relList->associatedAttributeValueChanged();
        if (didMutateRel)
            process();
        break;
    }
    case AttributeNames::hrefAttr: {
        URL url = getNonEmptyURLAttribute(hrefAttr);
        if (url == m_url)
            return;
        m_url = WTFMove(url);
        process();
        break;
    }
    case AttributeNames::typeAttr:
        if (newValue == m_type)
            return;
        m_type = newValue;
        process();
        break;
    case AttributeNames::sizesAttr:
        if (m_sizes)
            m_sizes->associatedAttributeValueChanged();
        process();
        break;
    case AttributeNames::blockingAttr:
        if (m_blockingList)
            m_blockingList->associatedAttributeValueChanged();
        process();
        break;
    case AttributeNames::mediaAttr: {
        auto media = newValue.string().convertToASCIILowercase();
        if (media == m_media)
            return;
        m_media = WTFMove(media);
        process();
        if (m_sheet && !isDisabled())
            m_styleScope->didChangeActiveStyleSheetCandidates();
        break;
    }
    case AttributeNames::disabledAttr:
        setDisabledState(!newValue.isNull());
        break;
    case AttributeNames::titleAttr:
        if (m_sheet && !isInShadowTree())
            m_sheet->setTitle(newValue);
        break;
    default:
        HTMLElement::attributeChanged(name, oldValue, newValue, attributeModificationReason);
        break;
    }
}

bool HTMLLinkElement::shouldLoadLink()
{
    return isConnected();
}

void HTMLLinkElement::setCrossOrigin(const AtomString& value)
{
    setAttributeWithoutSynchronization(crossoriginAttr, value);
}

String HTMLLinkElement::crossOrigin() const
{
    return parseCORSSettingsAttribute(attributeWithoutSynchronization(crossoriginAttr));
}

void HTMLLinkElement::setAs(const AtomString& value)
{
    setAttributeWithoutSynchronization(asAttr, value);
}

String HTMLLinkElement::as() const
{
    String as = attributeWithoutSynchronization(asAttr);
    if (equalLettersIgnoringASCIICase(as, "fetch"_s)
        || equalLettersIgnoringASCIICase(as, "image"_s)
        || equalLettersIgnoringASCIICase(as, "script"_s)
        || equalLettersIgnoringASCIICase(as, "style"_s)
        || (document().settings().mediaPreloadingEnabled() && (equalLettersIgnoringASCIICase(as, "video"_s) || equalLettersIgnoringASCIICase(as, "audio"_s)))
        || equalLettersIgnoringASCIICase(as, "track"_s)
        || equalLettersIgnoringASCIICase(as, "font"_s))
        return as.convertToASCIILowercase();
    return String();
}

void HTMLLinkElement::process()
{
    if (!isConnected()) {
        ASSERT(!m_sheet);
        return;
    }

    // Prevent recursive loading of link.
    if (m_isHandlingBeforeLoad)
        return;

    processInternalResourceLink();
    if (m_relAttribute.isInternalResourceLink)
        return;

    Ref document = this->document();
    LinkLoadParameters params {
        m_relAttribute,
        m_url,
        attributeWithoutSynchronization(asAttr),
        attributeWithoutSynchronization(mediaAttr),
        attributeWithoutSynchronization(typeAttr),
        attributeWithoutSynchronization(crossoriginAttr),
        attributeWithoutSynchronization(imagesrcsetAttr),
        attributeWithoutSynchronization(imagesizesAttr),
        nonce(),
        referrerPolicy(),
        fetchPriorityHint(),
    };

    m_linkLoader.loadLink(params, document);

    bool treatAsStyleSheet = false;
    if (m_relAttribute.isStyleSheet) {
        if (m_type.isNull())
            treatAsStyleSheet = true;
        else if (auto parsedContentType = ParsedContentType::create(m_type))
            treatAsStyleSheet = equalLettersIgnoringASCIICase(parsedContentType->mimeType(), "text/css"_s);
    }
    if (!treatAsStyleSheet)
        treatAsStyleSheet = document->settings().treatsAnyTextCSSLinkAsStylesheet() && m_type.containsIgnoringASCIICase("text/css"_s);

    LOG_WITH_STREAM(StyleSheets, stream << "HTMLLinkElement " << this << " process() - treatAsStyleSheet " << treatAsStyleSheet);

    if (m_disabledState != Disabled && treatAsStyleSheet && document->frame() && m_url.isValid()) {
        String charset = attributeWithoutSynchronization(charsetAttr);
        if (!PAL::TextEncoding { charset }.isValid())
            charset = document->charset();

        if (CachedResourceHandle cachedSheet = std::exchange(m_cachedSheet, nullptr)) {
            removePendingSheet();
            cachedSheet->removeClient(*this);
        }

        {
            bool previous = m_isHandlingBeforeLoad;
            m_isHandlingBeforeLoad = true;
            auto scopeExit = makeScopeExit([&] { m_isHandlingBeforeLoad = previous; });
            if (!shouldLoadLink())
                return;
        }

        m_loading = true;

        // Don't hold up render tree construction and script execution on stylesheets
        // that are not needed for the rendering at the moment.
        bool isActive = mediaAttributeMatches() && !isAlternate();
        addPendingSheet(isActive ? ActiveSheet : InactiveSheet);

        // Load stylesheets that are not needed for the rendering immediately with low priority.
        std::optional<ResourceLoadPriority> priority;
        if (!isActive)
            priority = DefaultResourceLoadPriority::inactiveStyleSheet;

        m_integrityMetadataForPendingSheetRequest = attributeWithoutSynchronization(HTMLNames::integrityAttr);

        ResourceLoaderOptions options = CachedResourceLoader::defaultCachedResourceOptions();
        options.nonce = nonce();
        options.sameOriginDataURLFlag = SameOriginDataURLFlag::Set;
        if (document->checkedContentSecurityPolicy()->allowStyleWithNonce(options.nonce))
            options.contentSecurityPolicyImposition = ContentSecurityPolicyImposition::SkipPolicyCheck;
        options.integrity = m_integrityMetadataForPendingSheetRequest;
        options.referrerPolicy = params.referrerPolicy;
        options.fetchPriorityHint = fetchPriorityHint();

        auto request = createPotentialAccessControlRequest(m_url, WTFMove(options), document, crossOrigin());
        request.setPriority(WTFMove(priority));
        request.setCharset(WTFMove(charset));
        request.setInitiator(*this);

        ASSERT_WITH_SECURITY_IMPLICATION(!m_cachedSheet);
        m_cachedSheet = document->protectedCachedResourceLoader()->requestCSSStyleSheet(WTFMove(request)).value_or(nullptr);

        if (CachedResourceHandle cachedSheet = m_cachedSheet)
            cachedSheet->addClient(*this);
        else {
            // The request may have been denied if (for example) the stylesheet is local and the document is remote.
            m_loading = false;
            sheetLoaded();
            notifyLoadedSheetAndAllCriticalSubresources(true);
        }

        return;
    }

    if (m_sheet) {
        // we no longer contain a stylesheet, e.g. perhaps rel or type was changed
        clearSheet();
        m_styleScope->didChangeActiveStyleSheetCandidates();
        return;
    }

#if ENABLE(APPLICATION_MANIFEST)
    if (isApplicationManifest()) {
        if (RefPtr loader = document->loader())
            loader->loadApplicationManifest({ });
        return;
    }
#endif // ENABLE(APPLICATION_MANIFEST)
}

void HTMLLinkElement::clearSheet()
{
    ASSERT(m_sheet);
    ASSERT(m_sheet->ownerNode() == this);
    m_sheet->clearOwnerNode();
    m_sheet = nullptr;
}

// https://html.spec.whatwg.org/multipage/links.html#process-internal-resource-link
void HTMLLinkElement::processInternalResourceLink(HTMLAnchorElement* anchor)
{
    if (document().wasRemovedLastRefCalled())
        return;

    if (!m_relAttribute.isInternalResourceLink) {
        unblockRendering();
        return;
    }

    if (!equalIgnoringFragmentIdentifier(m_url, document().url())) {
        unblockRendering();
        return;
    }

    RefPtr<Element> indicatedElement;
    // If the change originated from an anchor, then we can just check if that's
    // the right one instead doing a tree search using the name
    if (anchor) {
        if (anchor->name() == m_url.fragmentIdentifier())
            indicatedElement = anchor;
    } else
        indicatedElement = document().findAnchor(m_url.fragmentIdentifier());

    // FIXME: "is on a stack of open elements of an HTML parser whose associated Document is doc"
    if (document().readyState() == Document::ReadyState::Loading && isConnected() && mediaAttributeMatches() && blocking().contains("render"_s) && !indicatedElement) {
        if (!m_isRenderBlocking) {
            document().blockRenderingOn(*this);
            m_isRenderBlocking = true;
        }
        if (!m_expectIdTargetObserver)
            m_expectIdTargetObserver = makeUnique<ExpectIdTargetObserver>(makeAtomString(m_url.fragmentIdentifier()), *this);
    } else
        unblockRendering();
}

void HTMLLinkElement::unblockRendering()
{
    if (m_isRenderBlocking) {
        document().unblockRenderingOn(*this);
        m_isRenderBlocking = false;
    }
}

Node::InsertedIntoAncestorResult HTMLLinkElement::insertedIntoAncestor(InsertionType insertionType, ContainerNode& parentOfInsertedTree)
{
    HTMLElement::insertedIntoAncestor(insertionType, parentOfInsertedTree);
    if (!insertionType.connectedToDocument)
        return InsertedIntoAncestorResult::Done;

    m_styleScope = &Style::Scope::forNode(*this);
    m_styleScope->addStyleSheetCandidateNode(*this, m_createdByParser);

    return InsertedIntoAncestorResult::NeedsPostInsertionCallback;
}

void HTMLLinkElement::didFinishInsertingNode()
{
    m_url = getNonEmptyURLAttribute(hrefAttr);
    process();
}

void HTMLLinkElement::removedFromAncestor(RemovalType removalType, ContainerNode& oldParentOfRemovedTree)
{
    HTMLElement::removedFromAncestor(removalType, oldParentOfRemovedTree);
    if (!removalType.disconnectedFromDocument)
        return;

    m_linkLoader.cancelLoad();

    bool wasLoading = styleSheetIsLoading();

    if (m_sheet)
        clearSheet();

    if (wasLoading)
        removePendingSheet();

    if (m_styleScope) {
        m_styleScope->removeStyleSheetCandidateNode(*this);
        m_styleScope = nullptr;
    }

    processInternalResourceLink();
}

void HTMLLinkElement::finishParsingChildren()
{
    m_createdByParser = false;
    HTMLElement::finishParsingChildren();
}

void HTMLLinkElement::initializeStyleSheet(Ref<StyleSheetContents>&& styleSheet, const CachedCSSStyleSheet& cachedStyleSheet, MediaQueryParserContext context)
{
    // FIXME: originClean should be turned to false except if fetch mode is CORS.
    std::optional<bool> originClean;
    if (cachedStyleSheet.options().mode == FetchOptions::Mode::Cors)
        originClean = cachedStyleSheet.isCORSSameOrigin();

    if (m_sheet) {
        ASSERT(m_sheet->ownerNode() == this);
        m_sheet->clearOwnerNode();
    }

    m_sheet = CSSStyleSheet::create(WTFMove(styleSheet), *this, originClean);
    m_sheet->setMediaQueries(MQ::MediaQueryParser::parse(m_media, context));
    if (!isInShadowTree())
        m_sheet->setTitle(title());

    if (!m_sheet->canAccessRules())
        m_sheet->contents().setAsOpaque();
}

void HTMLLinkElement::setCSSStyleSheet(const String& href, const URL& baseURL, const String& charset, const CachedCSSStyleSheet* cachedStyleSheet)
{
    if (!isConnected()) {
        ASSERT(!m_sheet);
        return;
    }
    RefPtr frame = document().frame();
    if (!frame)
        return;

    // Completing the sheet load may cause scripts to execute.
    Ref<HTMLLinkElement> protectedThis(*this);

    if (!cachedStyleSheet->errorOccurred() && !matchIntegrityMetadata(*cachedStyleSheet, m_integrityMetadataForPendingSheetRequest)) {
        document().addConsoleMessage(MessageSource::Security, MessageLevel::Error, makeString("Cannot load stylesheet "_s, integrityMismatchDescription(*cachedStyleSheet, m_integrityMetadataForPendingSheetRequest)));

        m_loading = false;
        sheetLoaded();
        notifyLoadedSheetAndAllCriticalSubresources(true);
        return;
    }

    CSSParserContext parserContext(document(), baseURL, charset);
    auto cachePolicy = frame->loader().subresourceCachePolicy(baseURL);

    if (auto restoredSheet = const_cast<CachedCSSStyleSheet*>(cachedStyleSheet)->restoreParsedStyleSheet(parserContext, cachePolicy, frame->loader())) {
        ASSERT(restoredSheet->isCacheable());
        ASSERT(!restoredSheet->isLoading());
        initializeStyleSheet(restoredSheet.releaseNonNull(), *cachedStyleSheet, MediaQueryParserContext(document()));

        m_loading = false;
        sheetLoaded();
        notifyLoadedSheetAndAllCriticalSubresources(false);
        return;
    }

    auto styleSheet = StyleSheetContents::create(href, parserContext);
    initializeStyleSheet(styleSheet.copyRef(), *cachedStyleSheet, MediaQueryParserContext(document()));

    // FIXME: Set the visibility option based on m_sheet being clean or not.
    // Best approach might be to set it on the style sheet content itself or its context parser otherwise.
    if (!styleSheet.get().parseAuthorStyleSheet(cachedStyleSheet, &document().securityOrigin())) {
        m_loading = false;
        sheetLoaded();
        notifyLoadedSheetAndAllCriticalSubresources(true);
        return;
    }

    m_loading = false;
    styleSheet.get().notifyLoadedSheet(cachedStyleSheet);
    styleSheet.get().checkLoaded();

    if (styleSheet.get().isCacheable())
        const_cast<CachedCSSStyleSheet*>(cachedStyleSheet)->saveParsedStyleSheet(WTFMove(styleSheet));
}

bool HTMLLinkElement::styleSheetIsLoading() const
{
    if (m_loading)
        return true;
    if (!m_sheet)
        return false;
    return m_sheet->contents().isLoading();
}

DOMTokenList& HTMLLinkElement::sizes()
{
    if (!m_sizes)
        m_sizes = makeUniqueWithoutRefCountedCheck<DOMTokenList>(*this, sizesAttr);
    return *m_sizes;
}

bool HTMLLinkElement::mediaAttributeMatches() const
{
    if (m_media.isEmpty())
        return true;

    std::optional<RenderStyle> documentStyle;
    if (document().hasLivingRenderTree())
        documentStyle = Style::resolveForDocument(document());
    auto mediaQueryList = MQ::MediaQueryParser::parse(m_media, { document() });
    LOG(MediaQueries, "HTMLLinkElement::mediaAttributeMatches");

    MQ::MediaQueryEvaluator evaluator(document().frame()->view()->mediaType(), document(), documentStyle ? &*documentStyle : nullptr);
    return evaluator.evaluate(mediaQueryList);
}

void HTMLLinkElement::linkLoaded()
{
    m_loadedResource = true;
    if (!m_relAttribute.isLinkPrefetch || m_allowPrefetchLoadAndErrorForTesting)
        linkLoadEventSender().dispatchEventSoon(*this, eventNames().loadEvent);
}

void HTMLLinkElement::linkLoadingErrored()
{
    if (!m_relAttribute.isLinkPrefetch || m_allowPrefetchLoadAndErrorForTesting)
        linkLoadEventSender().dispatchEventSoon(*this, eventNames().errorEvent);
}

bool HTMLLinkElement::sheetLoaded()
{
    if (!styleSheetIsLoading()) {
        removePendingSheet();
        return true;
    }
    return false;
}

void HTMLLinkElement::dispatchPendingLoadEvents(Page* page)
{
    linkLoadEventSender().dispatchPendingEvents(page);
}

void HTMLLinkElement::dispatchPendingEvent(LinkEventSender* eventSender, const AtomString& eventType)
{
    ASSERT_UNUSED(eventSender, eventSender == &linkLoadEventSender());
    dispatchEvent(Event::create(eventType, Event::CanBubble::No, Event::IsCancelable::No));
}

DOMTokenList& HTMLLinkElement::relList()
{
    if (!m_relList)
        m_relList = makeUniqueWithoutRefCountedCheck<DOMTokenList>(*this, HTMLNames::relAttr, [](Document& document, StringView token) {
            return LinkRelAttribute::isSupported(document, token);
        });
    return *m_relList;
}

// https://html.spec.whatwg.org/multipage/semantics.html#dom-link-blocking
DOMTokenList& HTMLLinkElement::blocking()
{
    if (!m_blockingList) {
        m_blockingList = makeUniqueWithoutRefCountedCheck<DOMTokenList>(*this, HTMLNames::blockingAttr, [](Document&, StringView token) {
            if (equalLettersIgnoringASCIICase(token, "render"_s))
                return true;
            return false;
        });
    }
    return *m_blockingList;
}

void HTMLLinkElement::notifyLoadedSheetAndAllCriticalSubresources(bool errorOccurred)
{
    m_loadedResource = !errorOccurred;
    linkLoadEventSender().dispatchEventSoon(*this, m_loadedResource ? eventNames().loadEvent : eventNames().errorEvent);
}

void HTMLLinkElement::startLoadingDynamicSheet()
{
    // We don't support multiple active sheets.
    ASSERT(m_pendingSheetType < ActiveSheet);
    addPendingSheet(ActiveSheet);
}

bool HTMLLinkElement::isURLAttribute(const Attribute& attribute) const
{
    return attribute.name().localName() == hrefAttr || HTMLElement::isURLAttribute(attribute);
}

URL HTMLLinkElement::href() const
{
    return document().completeURL(attributeWithoutSynchronization(hrefAttr));
}

const AtomString& HTMLLinkElement::rel() const
{
    return attributeWithoutSynchronization(relAttr);
}

AtomString HTMLLinkElement::target() const
{
    return attributeWithoutSynchronization(targetAttr);
}

const AtomString& HTMLLinkElement::type() const
{
    return attributeWithoutSynchronization(typeAttr);
}

std::optional<LinkIconType> HTMLLinkElement::iconType() const
{
    return m_relAttribute.iconType;
}

static bool mayFetchResource(LinkRelAttribute relAttribute)
{
    // https://html.spec.whatwg.org/multipage/links.html#linkTypes
    return relAttribute.isStyleSheet
        || relAttribute.isLinkModulePreload
        || relAttribute.isLinkPreload
#if ENABLE(APPLICATION_MANIFEST)
        || relAttribute.isApplicationManifest
#endif
        || !!relAttribute.iconType;
}

void HTMLLinkElement::addSubresourceAttributeURLs(ListHashSet<URL>& urls) const
{
    HTMLElement::addSubresourceAttributeURLs(urls);

    if (!mayFetchResource(m_relAttribute))
        return;

    // Append the URL of this link element.
    addSubresourceURL(urls, href());

    if (RefPtr styleSheet = this->sheet()) {
        styleSheet->contents().traverseSubresources([&] (auto& resource) {
            urls.add(resource.url());
            return false;
        });
    }
}

void HTMLLinkElement::addPendingSheet(PendingSheetType type)
{
    if (type <= m_pendingSheetType)
        return;
    m_pendingSheetType = type;

    if (m_pendingSheetType == InactiveSheet)
        return;
    ASSERT(m_styleScope);
    m_styleScope->addPendingSheet(*this);
}

void HTMLLinkElement::removePendingSheet()
{
    PendingSheetType type = m_pendingSheetType;
    m_pendingSheetType = Unknown;

    if (type == Unknown)
        return;

    ASSERT(m_styleScope);
    if (type == InactiveSheet) {
        // Document just needs to know about the sheet for exposure through document.styleSheets
        m_styleScope->didChangeActiveStyleSheetCandidates();
        return;
    }

    m_styleScope->removePendingSheet(*this);
}

void HTMLLinkElement::setReferrerPolicyForBindings(const AtomString& value)
{
    setAttributeWithoutSynchronization(referrerpolicyAttr, value);
}

String HTMLLinkElement::referrerPolicyForBindings() const
{
    return referrerPolicyToString(referrerPolicy());
}

ReferrerPolicy HTMLLinkElement::referrerPolicy() const
{
        return parseReferrerPolicy(attributeWithoutSynchronization(referrerpolicyAttr), ReferrerPolicySource::ReferrerPolicyAttribute).value_or(ReferrerPolicy::EmptyString);
}

String HTMLLinkElement::debugDescription() const
{
    return makeString(HTMLElement::debugDescription(), ' ', type(), ' ', href().string());
}

void HTMLLinkElement::setFetchPriorityForBindings(const AtomString& value)
{
    setAttributeWithoutSynchronization(fetchpriorityAttr, value);
}

String HTMLLinkElement::fetchPriorityForBindings() const
{
    return convertEnumerationToString(fetchPriorityHint());
}

RequestPriority HTMLLinkElement::fetchPriorityHint() const
{
    if (document().settings().fetchPriorityEnabled())
        return parseEnumerationFromString<RequestPriority>(attributeWithoutSynchronization(fetchpriorityAttr)).value_or(RequestPriority::Auto);
    return RequestPriority::Auto;
}

} // namespace WebCore
