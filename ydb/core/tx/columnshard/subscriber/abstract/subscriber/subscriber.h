#pragma once
#include <ydb/core/tx/columnshard/subscriber/events/tables_erased/event.h>
#include <ydb/core/tx/columnshard/subscriber/events/transaction_completed/event.h>
#include <ydb/core/tx/columnshard/subscriber/events/writes_completed/event.h>
#include <memory>
#include <set>

namespace NKikimr::NColumnShard::NSubscriber {

class ISubscriber {
public:
    enum class EEventHandlingResult {
        //The order is important for result compostion
        Finished,
        StillWaiting,
        EventSetChanged, //implied StillWaiting for new event types
    };
    virtual EEventHandlingResult OnEvent(const std::shared_ptr<ISubscriptionEvent>& ev) = 0;
    virtual std::set<EEventType> GetEventTypes() const = 0;
    virtual bool IsFinished() const = 0;
    virtual ~ISubscriber() = default;
};

class TSubscriberBase: public ISubscriber {
protected:
    virtual EEventHandlingResult DoOnEvent(const NSubscriber::TEventTablesErased&) { return EEventHandlingResult::StillWaiting; };
    virtual EEventHandlingResult DoOnEvent(const NSubscriber::TEventTransactionCompleted&){ return EEventHandlingResult::StillWaiting; };
    virtual EEventHandlingResult DoOnEvent(const NSubscriber::TEventWritesCompleted&) { return EEventHandlingResult::StillWaiting; };
public:
    EEventHandlingResult OnEvent(const std::shared_ptr<ISubscriptionEvent>& ev) override {
        switch(ev->GetType()) {
            case EEventType::Undefined:
                AFL_VERIFY(false);
            case EEventType::TablesErased: //delete me
                return DoOnEvent(static_cast<const NSubscriber::TEventTablesErased&>(*ev.get()));
            case EEventType::TransactionCompleted: //delete me
                return DoOnEvent(static_cast<const NSubscriber::TEventTransactionCompleted&>(*ev.get()));
            case EEventType::WritesCompleted: //delete me
                return DoOnEvent(static_cast<const NSubscriber::TEventWritesCompleted&>(*ev.get()));
            case EEventType::IndexationCompleted:
                return DoOnEvent(static_cast<const NSubscriber::TEventWritesCompleted&>(*ev.get())); //TODO fixme
            case EEventType::DataLockAccuired:
                return EEventHandlingResult::StillWaiting; //FIX me
        }
    }
};

} //namespace NKikimr::NColumnShard::NSubscriber