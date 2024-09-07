#pragma once
#include "subscriber.h"


namespace NKikimr::NColumnShard::NSubscriber {

class TParallelWaitSubscriber: public TSubscriberBase {
    std::vector<std::shared_ptr<NSubscriber::ISubscriber>> Subscribers;
public:
    TParallelWaitSubscriber(std::initializer_list<std::shared_ptr< NSubscriber::ISubscriber>> subscribers)
        : Subscribers(subscribers) 
    {
    }
    bool IsFinished() const override {
        return std::all_of(cbegin(Subscribers), cend(Subscribers), [](auto s){ return s->IsFinished();});
    }
    std::set<NSubscriber::EEventType> GetEventTypes() const override {
        return std::accumulate(cbegin(Subscribers), cend(Subscribers), std::set<EEventType>{}, [](auto a, const auto& s){ 
            const auto& events = s->GetEventTypes();
            a.insert(cbegin(events), cend(events)); 
            return a;
        });
    }
    EEventHandlingResult OnEvent(const std::shared_ptr<NSubscriber::ISubscriptionEvent>& ev) override {
        //For the sake of simplicity, we don't filter events consumed by each particular subscriber. All subscribers are called with every incoming event
        //Can be improved in case of poor perfomance
        enum EEventHandlingResult result = EEventHandlingResult::Finished;
        for (auto s: Subscribers) {
            AFL_VERIFY(!!s);
            const auto r = s->OnEvent(ev);
            if (r > result) { //Explore the ordering of EEventHandlingResult values
                result = r;
            }
            if (r == EEventHandlingResult::Finished) {
                s.reset();
            }
        }
        Subscribers.erase(std::remove(begin(Subscribers), end(Subscribers), std::shared_ptr<NSubscriber::ISubscriber>{}), end(Subscribers));
        return result;
    }
};

class TSequentialWaitSubscriber: public TSubscriberBase {
public:
    using TSubscriberCreator = std::function<std::shared_ptr<NSubscriber::ISubscriber>()>;
    std::shared_ptr<NSubscriber::ISubscriber> Subscriber;
    TSubscriberCreator NextSubscriberCreator;
    
public:
    TSequentialWaitSubscriber(std::shared_ptr<NSubscriber::ISubscriber> subscriber, TSubscriberCreator nextSubscriberCreator)
        : Subscriber(subscriber)
        , NextSubscriberCreator(nextSubscriberCreator) 
    {
    }
    bool IsFinished() const override {
        return Subscriber && !Subscriber->IsFinished();
    }
    std::set<NSubscriber::EEventType> GetEventTypes() const override {
        if (Subscriber) {
            return Subscriber->GetEventTypes();
        } else {
            return {};
        }
    }
    EEventHandlingResult OnEvent(const std::shared_ptr<NSubscriber::ISubscriptionEvent>& ev) override {
        AFL_VERIFY(!IsFinished());
        const auto r = Subscriber->OnEvent(ev);
        if (r == EEventHandlingResult::Finished) {
            Subscriber = NextSubscriberCreator();
            if (Subscriber) {
                return EEventHandlingResult::EventSetChanged;
            } else {
                return EEventHandlingResult::Finished;
            }         
        }
        return r;
    }
};

} //namespace NKikimr::NColumnShard::NSubscriber