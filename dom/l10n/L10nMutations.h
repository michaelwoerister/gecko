#ifndef mozilla_dom_l10n_L10nMutations_h__
#define mozilla_dom_l10n_L10nMutations_h__

#include "nsRefreshDriver.h"
#include "nsStubMutationObserver.h"
#include "nsTHashtable.h"
#include "mozilla/dom/DOMLocalization.h"

namespace mozilla {
namespace dom {

/**
 * L10nMutations manage observing roots for localization
 * changes and coalescing pending translations into
 * batches - one per animation frame.
 */
class L10nMutations final : public nsStubMutationObserver,
                            public nsARefreshObserver {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_CLASS_AMBIGUOUS(L10nMutations, nsIMutationObserver)
  NS_DECL_NSIMUTATIONOBSERVER_CONTENTAPPENDED
  NS_DECL_NSIMUTATIONOBSERVER_CONTENTINSERTED
  NS_DECL_NSIMUTATIONOBSERVER_ATTRIBUTECHANGED

  explicit L10nMutations(DOMLocalization* aDOMLocalization);

  /**
   * Pause root observation.
   * This is useful for injecting already-translated
   * content into an observed root, without causing
   * superflues translation.
   */
  void PauseObserving();

  /**
   * Resume root observation.
   */
  void ResumeObserving();

  /**
   * Disconnect roots, stop refresh observer
   * and break the cycle collection deadlock
   * by removing the reference to mDOMLocalization.
   */
  void Disconnect();

  /**
   * Called when PresShell gets created for the document.
   * If there are already pending mutations, this
   * will schedule the refresh driver to translate them.
   */
  void OnCreatePresShell();

 protected:
  bool mObserving = false;
  bool mRefreshObserver = false;
  RefPtr<nsRefreshDriver> mRefreshDriver;
  DOMLocalization* mDOMLocalization;

  // The hash is used to speed up lookups into mPendingElements.
  nsTHashtable<nsRefPtrHashKey<Element>> mPendingElementsHash;
  nsTArray<RefPtr<Element>> mPendingElements;

  virtual void WillRefresh(mozilla::TimeStamp aTime) override;

  void StartRefreshObserver();
  void StopRefreshObserver();
  void L10nElementChanged(Element* aElement);
  void FlushPendingTranslations();

 private:
  ~L10nMutations() {
    StopRefreshObserver();
    MOZ_ASSERT(!mDOMLocalization,
               "DOMLocalization<-->L10nMutations cycle should be broken.");
  }
};

}  // namespace dom
}  // namespace mozilla

#endif  // mozilla_dom_l10n_L10nMutations_h__
