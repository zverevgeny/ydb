#pragma once

#include "flat_page_iface.h"
#include "flat_sausage_fetch.h"
#include "flat_fwd_misc.h"
#include "shared_handle.h"

namespace NKikimr {
namespace NTable {
    using EPage = NPage::EPage;
    using TPageId = NPage::TPageId;

namespace NFwd {

    struct TPage;

    class IPageLoadingQueue {
    public:
        virtual ~IPageLoadingQueue() = default;

        virtual ui64 AddToQueue(TPageId pageId, EPage type) = 0;
    };

    class IPageLoadingLogic {
    public:
        struct TResult {
            const TSharedData *Page;
            bool Grow; /* Should give more pages on Forward() */
            bool Need; /* Is vital to client to make progress */
        };

        virtual ~IPageLoadingLogic() = default;

        virtual TResult Get(IPageLoadingQueue *head, TPageId pageId, EPage type, ui64 lower) = 0;
        virtual void Forward(IPageLoadingQueue *head, ui64 upper) = 0;
        virtual void Fill(NPageCollection::TLoadedPage& page, NSharedCache::TSharedPageRef sharedPageRef, EPage type) = 0;

        IPageLoadingQueue* Head = nullptr; /* will be set outside of IPageLoadingLogic impl */
        TStat Stat;
    };

}
}
}
