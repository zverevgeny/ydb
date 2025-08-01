#pragma once
#include "defs.h"
#include <compare>
#include <concepts>
#include <util/generic/string.h>
#include <util/string/builder.h>

#include <google/protobuf/message.h>

template <typename T, typename Tag> class TIdWrapper {
private:
    T Raw = {};

public:
    using Type = T;
    using TTag = Tag;

    constexpr TIdWrapper() noexcept {}

    TIdWrapper(TIdWrapper &&value) = default;

    TIdWrapper(const TIdWrapper &other) = default;

    TIdWrapper &operator=(const TIdWrapper &value) = default;

    TIdWrapper &operator=(TIdWrapper &&value) = default;

    TString ToString() const { return TStringBuilder() << Raw; }

    template<typename TProto>
    void CopyToProto(TProto *message, void (TProto::*pfn)(T value)) const {
        std::invoke(pfn, message, Raw);
    }

    static constexpr TIdWrapper FromValue(T value) noexcept {
        TIdWrapper id;
        id.Raw = value;
        return id;
    }

    template <typename TType, typename TProto>
    static constexpr TIdWrapper FromProto(const TType *message, TProto (TType::*pfn)() const) {
        return FromValue(std::invoke(pfn, message));
    }

    static constexpr TIdWrapper Zero() noexcept { return TIdWrapper(); }

    TIdWrapper &operator+=(const T &other) {
        Raw += other.Raw;
        return *this;
    }

    friend TIdWrapper operator+(const TIdWrapper &first,
                                const T &second) Y_WARN_UNUSED_RESULT {
        return TIdWrapper(first->Raw + second);
    }

    TIdWrapper &operator++() {
        Raw++;
        return *this;
    }

    TIdWrapper operator++(int) {
        TIdWrapper old = *this;
        operator++();
        return old;
    }

    friend std::ostream &operator<<(std::ostream &out, TIdWrapper &id) {
        return out << id.Raw;
    }

    friend IOutputStream &operator<<(IOutputStream &out, const TIdWrapper &id) {
        return out << id.Raw;
    }

    friend std::strong_ordering operator<=>(const TIdWrapper&, const TIdWrapper&) = default;

    T GetRawId() const { return Raw; }

    static const TIdWrapper Min() { return FromValue(::Min<T>()); }
    static const TIdWrapper Max() { return FromValue(::Max<T>()); }

    friend std::hash<TIdWrapper<T, Tag>>;

    friend THash<TIdWrapper<T, Tag>>;
};

template<typename T, typename Tag>
struct std::hash<TIdWrapper<T, Tag>> {
    std::size_t operator()(const TIdWrapper<T, Tag> &id) const {
        return std::hash<T>{}(id.Raw);
    }
};

template<typename T, typename Tag>
struct THash<TIdWrapper<T, Tag>> {
    std::size_t operator()(const TIdWrapper<T, Tag> &id) const {
        return THash<T>{}(id.Raw);
    }
};
