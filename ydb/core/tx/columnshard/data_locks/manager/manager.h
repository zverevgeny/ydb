#pragma once
#include <ydb/core/tx/columnshard/data_locks/locks/abstract.h>
#include <ydb/core/tx/columnshard/subscriber/abstract/subscriber/subscriber.h>
#include <util/generic/hash.h>
#include <util/generic/string.h>
#include <optional>

namespace NKikimr::NOlap::NDataLocks {

template<typename T>
concept TLocable = 
    std::same_as<T, TPortionInfo> || 
//    std::same_as<T, TGranuleMeta> ||
    std::same_as<T, ui64>; //table pathId

enum class ELockType {
    Exclusive, //at most one lock of this type can exist on an object at every instant
    Shared, //any number of locks of this type can co-exist on the same object
};

enum class ELockCategory {
    Existense, //i.e If someone holds this category exclusive lock on a table by pathId, a new table can'not created with such pathId
    Transaction, //New (distibuted) transaction creation. If someone holds this category exclusive lock on an object, then a new transaction for locked object can'not be created
    Background, //Background
};


// Какие процессы требуют блокировки и ожидания:
// 1. Удаление таблицы. Возникает при перешардировании(ShardCount/= 2, ShardCount *=2). 
// Можно решить через лок с категорией Existense. Каждая таблица в TableManager'е держит эксклюзивный лок. 
// При необходимости создать новую таблицу ожидаем захват эксклюзивного лока на Propose. Получив лок передаём его в TableManager и там создаём таблицу, которая будет держать этот лок
// 2. Ожидание завершения всех начатых транзакций над данной таблицей;
// Можно решить через лок с категорией Transaction. Транзакции, которые не требуют эксклюизвного выполнения на Propose пытаются захватывают shared lock на pathId. Можно настраивать поведение при невозможности захватить лок(реджектить транзакцию или дожидаться), сейчас реджектим.
// Транзакции, которые требуют эксклюзивного выполнения (сейчас это MoveTable), пытаются захватить эксклюзивный Transaction лок на pathId. При



class TLockCategories {
public:
    TLockCategories(std::initializer_list<ELockCategory>);
    //TODO impl
};


template<typename T>
concept TLocableIterator = std::forward_iterator<T> && TLocable<T>;


class TManager {
private:
    THashMap<TString, std::shared_ptr<ILock>> ProcessLocks;
    std::shared_ptr<TAtomicCounter> StopFlag = std::make_shared<TAtomicCounter>(0);
    void UnregisterLock(const TString& processId);
public:
    TManager() = default;

    void Stop();

    class TGuard {
    private:
        const TString ProcessId;
        std::shared_ptr<TAtomicCounter> StopFlag;
        bool Released = false;
    public:
        TGuard() = default; //delete me
        TGuard(const TString& processId, const std::shared_ptr<TAtomicCounter>& stopFlag)
            : ProcessId(processId)
            , StopFlag(stopFlag)
        {

        }

        void AbortLock();

        ~TGuard();

        void Release(TManager& manager);
    };


    //onAccuired set to null nullopt means no waiting(try lock)
    template<TLocable T>
    std::optional<TGuard> Lock(const T& obj, ELockType type, TLockCategories categories, std::optional<std::function<void(TGuard&&)>> onAccuired) {
        //TODO implement me
        Y_UNUSED(obj);
        Y_UNUSED(type);
        Y_UNUSED(categories);
        Y_UNUSED()
        //onAccuired(TGuard{});
        return TGuard{}; 
    }

    //onAccuired set to null nullopt means no waiting(try lock)
    template<TLocableIterator TIterator>
    std::optional<TGuard> Lock(TIterator begin, TIterator end, ELockType type, TLockCategories categories, std::optional<std::function<void(TGuard&&)>> onAccuired) {
        //TODO implement me
        Y_UNUSED(begin, end);
        Y_UNUSED(type);
        Y_UNUSED(categories);
        //onAccuired(TGuard{});
        return TGuard{}; 
    }

    //Synonym for Lock() with no onAccurred to improve readability
    template<TLocable T>
    std::optional<TGuard> TryLock(const T& obj, ELockType type, TLockCategories categories){
        return Lock(obj, type, categories, std::nullopt);
    }
    //Synonym for Lock() with no onAccurred to improve readability
    template<TLocableIterator TIterator>
    std::optional<TGuard> TryLock(TIterator begin, TIterator end, ELockType type, TLockCategories categories){
        return Lock(begin, end, type, categories, std::nullopt);
    }

};


struct TEventDataLockAccuired: public NKikimr::NColumnShard::NSubscriber::ISubscriptionEvent {
    using TBase = ISubscriptionEvent;
    TManager::TGuard Guard;
    virtual TString DoDebugString() const override;

public:
    TEventDataLockAccuired(TManager::TGuard&& guard)
        : TBase( NKikimr::NColumnShard::NSubscriber::EEventType::DataLockAccuired)
        , Guard(std::move(guard))
    {
    }
    TManager::TGuard ExtractGuard() {
        return std::move(Guard);
    }
};


template<TLocable T>
class TWaitLock: public NColumnShard::NSubscriber::TSubscriberBase {
    std::optional<T> Obj;
    std::function<void(TManager::TGuard&&)> OnFinish;
public:
    TWaitLock(TManager& dataLockManager, const T& obj, ELockType type, TLockCategories categories, std::function<void(TManager::TGuard&&)> onFinish)
        : Obj(obj)
        , OnFinish(onFinish)
    {
        if (auto guard = dataLockManager.Lock(obj, type, categories)) {
            onFinish(guard);
        }
    }
    std::set<NColumnShard::NSubscriber::EEventType> GetEventTypes() const override {
        return { NColumnShard::NSubscriber::EEventType::DataLockAccuired };
    }
    bool IsFinished() const override {
        return !Obj.HasValue();
    }
    EEventHandlingResult DoOnEvent(const NColumnShard::NSubscriber::TEventDataLockAccuired& ev) override {
        Path.erase(ev.GetTxId());
        return EEventHandlingResult::StillWaiting;
    }
};

}