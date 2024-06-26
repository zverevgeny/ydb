# [library/cpp/yt/string]

## Structure

This library provides ways of printing data structures as well as some helpers methods to work with strings.

### `TStringBuilder` [library/cpp/yt/string/string_builder.h]

String formatter with dynamic buffer which supports strings, chars and arbitrary Format expressions (see below).
```cpp
TString HelloWorld()
{
    TStringBuilder builder;
    builder.AppendString("Hello,"); // <- Dynamic allocation of max(minSize, str.len()) bytes.
    builder.AppendChar(' ');
    builder.AppendFormat("World %v!", 42); // See Format section below
    return builder.Flush(); // Hello, World 42!
}
```

### `TRawFormatter` [library/cpp/yt/string/raw_formatter.h]

String formatter with static buffer which is stored on stack frame. Supports strings, chars, numbers and guids.
```cpp
TString HelloWorld(TGuid guid) // guid = "1-2-3-4"
{
    TRawFormatter<42> builder; // <- Buffer size is set right away. Never allocates.
    builder.AppendString("Hello");
    builder.AppendChar(' ');
    builder.AppendString("World ");
    builder.AppendGuid(guid);
    builder.AppendChar(' ');
    builder.AppendNumber(42);
    builder.AppendChar('!');
    return TString(builder.GetBuffer()); // Hello World 1-2-3-4 42!
}
```

Attempt to append string which results in buffer overflow truncates the string
```cpp
TString LongMessage()
{
    TRawFormatter<7> builder;
    builder.AppendString("Hello World!");
    return TString(builder.GetBuffer()); // Hello W
}
```

### `Format` [library/cpp/yt/string/format.h]

Universal way of generating strings in a fashion similar to `printf` with flags support.
```cpp
Format("Hello, World %d!", 42); // Hello, World 42!
```

Currently all std flags are supported via fallback to `printf` (Note: this is subject to change. We might remove support of the majority of flags in the future to reduce complexity on the user side). We additionally support "Universal" conversion specifier -- "v" which prints value in a certain default way.
```cpp
Format("Hello, World %v!", 42); // Hello, World 42!

Format("Value is %v", "MyValue"); // Value is MyValue

Format("Vector is %v", std::vector{1, 2, 3}); // Vector is [1, 2, 3]
```

"l" specifier can be applied to enums and bools to emit them as their lowercase versions:
```cpp
DEFINE_ENUM(EMyEnum,
((MyValue1)     (42))
((AnotherValue) (41))
);

Format("%v", true); // True
Format("%v", EMyEnum::MyValue1); // MyValue1

Format("%lv", true); // true
Format("%lv", EMyEnum::MyValue); // my_value1
```

"q" and "Q" specifiers wrap output into quotations marks ' and " respectively. If the same quotation marks are detected inside the formattable value, they are replaced by their "\\"-version:
```cpp
Format("%Qv", true); // "True"
Format("%qv", true); // 'true'
Format("%Qv", "\"Hello World\""); // "\"Hello World\""

// std::array{"MyValue1", "AnotherValue"}
auto names = TEnumTraits<EMyEnum>::GetDomainNames();
Format("%Qv", names); // "[\"MyValue1\", \"AnotherValue\"]"
```

`FormatterWrapper` allows conditional writes into the string:
```cpp
NYT::Format(
    "Value is %v%v",
    42,
    MakeFormatterWrapper([&] (auto* builder) {
        If (PossiblyMissingInfo_) {
            builder->AppendString(", PossiblyMissingInfo: ");
            FormatValue(builder, PossiblyMissingInfo_, "v");
        }
    }));
```

`FormatVector` allows treating range of values as a generator coroutine returning values for each placeholder:
```cpp
FormatVector("One: %v, Two: %v, Three: %v", {1, 2, 3})
// One: 1, Two: 2, Three: 3
```

### Customising Format

By default type is not Formattable:
```cpp
struct TMyStruct
{ };

static_assert(!CFormattable<TMyStruct>);
Format("%v", TMyStruct{}); // <- Results in CE
```
Compiler error looks like this:
```cpp
ROOT/library/cpp/yt/string/unittests/format_ut.cpp:46:36: error: call to consteval function 'NYT::TBasicStaticFormat<NYT::(anonymous namespace)::TMyStruct>::TBasicStaticFormat<char[3]>' is not a constant expression
[[maybe_unused]] auto val = Format("%v", TMyStruct{});
                                   ^
ROOT/library/cpp/yt/string/format_string-inl.h:38:17: note: non-constexpr function 'CrashCompilerClassIsNotFormattable<NYT::(anonymous namespace)::TMyStruct>' cannot be used in a constant expression
                CrashCompilerClassIsNotFormattable<std::tuple_element_t<Idx, TTuple>>();
                ^
ROOT/library/cpp/yt/string/format_string-inl.h:36:10: note: in call to '&[] {
    if (!CFormattable<std::tuple_element_t<0UL, TTuple>>) {
        CrashCompilerClassIsNotFormattable<std::tuple_element_t<0UL, TTuple>>();
    }
}->operator()()'
...
```

First line contains the source location where the error occured. Second line contains the function name `CrashCompilerClassIsNotFormattable<NYT::(anonymous namespace)::TMyStruct>` which name is the error and template argument is the errorneos type. There are some more lines which would contain incomprehensible garbage --- don't bother reading it. Other compiler errors generated by static analyser (see below) follow the same structure.

In order to support printing custom type, one must create an overload of `FormatValue` function. If everything is done correctly, concept `CFormattable<T>` should be satisfied and the value printed accordingly. 

```cpp
struct TMyStruct
{ };

void FormatValue(TStringBuilderBase* builder, const TMyStruct& /*val*/, TStringBuf /*spec*/)
{
    builder->AppendString(TStringBuf("TMyStruct"));
}

static_assert(CFormattable<TMyStruct>);
Format("%v", TMyStruct{}); // "TMyStruct"
```

First argument is already known builder (technically, the part of builder which can be written into, but not flushed). Second argument is the value to be formatted and the `spec` is the set of flags to be applied during the formatting. Spec must not be empty or contain the introductory symbol '%'!
```cpp
struct TMyPair
{
    int Key;
    TString Value;
};

void FormatValue(TStringBuilderBase* builder, const TMyPair& pair, TStringBuf spec)
{
    // We shall support an extra flag -- 'k' which forces pair to be printed differently
    bool concat = false;
    
    for (auto c : spec) {
        concat |= (c == 'k');
    }
    
    if (concat) {
        builder->AppendFormat("%v_%v", Key, Value);
    } else {
        builder->AppendFormat("{%v: %v}", Key, Value);
    }
};

// Required for static analysis (see section below)
// If you don't add extra specifiers you can ignore this part.
template <>
struct NYT::TFormatArg<TMyPair>
    : public NYT::TFormatArgBase
{
    static constexpr auto ConversionSpecifiers = TFormatArgBase::ConversionSpecifiers;

    static constexpr auto FlagSpecifiers = 
        TFormatArgBase::ExtendConversion</*Hot*/ true, 1, std::array{'k'}>();
};

Format("%v", TMyPair{42, "Hello"});
// spec is "v"
// output is {42: Hello}

Format("%kv", TMyPair{42, "Hello"});
// spec is "kv"
// output is 42_Hello
```

`TRuntimeFormat` is required if you want to use a non-constexpr value as a format string:
```cpp
cosntexpr TStringBuf fmtGood1 = "Hello, %v";
const char* fmtBad = "Hello, %v";
TRuntimeFormat fmtGood2{fmtBad};

Format(fmtGood1, "World!"); // Hello, World
Format(fmdBad, "World!"); // CE --- call to consteval function is not constexpr
Format(fmtGood2, "World!"); // Hello, World
```

### Static analysis (since clang-16)

If format string can bind to `TFormatString` (that is, it is a constexpr string_view or a literal) then static analysis on supplied args is performed.

#### How to disable

Per-file: `#define YT_DISABLE_FORMAT_STATIC_ANALYSIS` (see [library/cpp/yt/string/format_string.h] for up to date macro name). 

#### What is checked

Static analyser checks if the number of specifier sequences matches the number of arguments supplied. Validity of specifier sequences if checked per argument (that specifier sequence is either "%%" or starts with "%", ends with one of the conversion specifiers and contains only the flag specifiers in the middle). Lists of conversion specifiers and flags specifiers are customisation points (see [library/cpp/yt/string/format_arg.h]).

#### Customising static analysis

We have already seen that `TMyPair` additionally required specialization of `TFormatArg` struct in order to work. Unless you want to change the list of allowed specifiers, default definition of `TFormatArg<T>` will be sufficient. We want to add an extra flag specifier for `TMyPair` and thus we must specialize `NYT::TFormatArg<TMyPair>`:

```cpp
template <>
struct NYT::TFormatArg<TMyPair>
    : public NYT::TFormatArgBase // Contains default sets of specifiers and some convenience tools for customization.
{
    // Technically not required as it is present in base. Here written for exposition.
    static constexpr auto ConversionSpecifiers = TFormatArgBase::ConversionSpecifiers;
    
    // Adds 'k' flag to the list of default specifiers
    // 'Hot' means that we prepend specifier since we expect
    // it to be used frequently. This speeds up the compilation a little.
    static constexpr auto FlagSpecifiers = 
        TFormatArgBase::ExtendConversion</*Hot*/ true, 1, std::array{'k'}>();
};
```

Now we are able to print the value as format analyser is aware of the new flag 'k'. If we wanted to, we could remove the rest of the default specifiers provided by `TFormatArgBase`, since most of them might not make any sence for your type.

You can use `TFormatArg` + `FormatValue` to fully support format decorators:
```cpp
template <class T>
struct TDecorator
{
    T Value;  
};

template <class T>
struct NYT::TFormatArg<TDecorator<T>>
    : public NYT::TFormatArgBase
{
    static constexpr auto ConversionSpecifiers = TFormatArg<T>::ConversionSpecifiers;
    static constexpr auto FlagSpecifiers =
        TFormatArgBase::ExtendConversion</*Hot*/ true, 1, std::array{'D'}, /*TFrom*/ T>::ExtendConversion();
};

template <class T>
void FormatValue(NYT::TStringBuilderBase* builder, const TDecorator<T>& value, TStringBuf spec)
{
    bool append = (spec[0] == 'D');
    
    if (append) {
        builder->AppendString("TDecorator value: ");
        FormatValue(builder, value.Value, TStringBuf(&spec[1], spec.size() - 1));
        return;
    }
    
    FormatValue(builder, value.Value, spec);
}

Format("Testing: %v", TDecorator{TMyPair{42, "Hello"}});
// Testing: {42, Hello}
Format("Testing: %Dv", TDecorator{TMyPair{42, "Hello"}});
// Testing: TDecorator value: {42, Hello}
Format("Testing: %Dkv", TDecorator{TMyPair{42, "Hello"}});
// Testing: TDecorator value: 42_Hello
```

### ToString auto generation

For names inside namespaces enclosing `NYT` and `std` we automatically generate overload of `ToString` which uses `FormatValue` function if there is such a function. In examples below we assume that `CFormattable` holds true for each type:
```cpp
auto val = ToString(NYT::TMyPair{42, "Hello"}}); // Works since TMyPair comes from namespace NYT;

auto val = ToString(std::optional{NYT::TMyPair{42, "Hello"}}}); // Works since optional comes from namespace std

auto val = ToString(NYT::NOrm::::NClient::NObjects::TObjectKey{}); // Works since NOrm::::NClient::NObjects enclose namespace NYT

auto val = ToString(NMyNs::TMyType{}); // Falls back to util ToString because NMyNs encloses neither std nor NYT. Fate is unknown.

auto val = ToString(NMyNS::TMyContainer<NYT::TMyPair>{}); // Falls back to util ToString because NMyNs encloses neither std nor NYT (we don't care about template parameters). Fate is unknown.

auto val = NYT::ToString(NMyNs::TMyType{}); // Works.

auto val = NYT::ToString(NMyNS::TMyContainer<NYT::TMyPair>{}); // Also works.

{
    using ::ToString; // Irrelevant since NYT::ToString is more constrained.
    using NYT::ToString;
    auto val = ToString(NMyNS::TMyContainer<NYT::TMyPair>{}); // Also works.
}
```

### Implementing `FormatValue` via `ToString`

One thing you may attempt to do is to use already defined `ToString` method to implement `FormatValue`. There are two cases for this:
    1. You have an overload of `ToString` visible from the inside of `FormatValue` which is not the util default overload. In this case you are fine.
    2. You rely on util `ToString` overload. In this case you hit an infinite recursion loop if name of your type comes from `std` or `NYT` namespaces. We strongly recommend that you stop relying on util `ToString` and simply write `FormatValue` from scratch. Should this be impossible, use `NYT::ToStringIgnoringFormatValue(const T&)` which implements default `ToString` mechanism (via `operator <<`) from util. This method has a different name hence it will break the recursion loop.

### Format extensions.

There are some types from util and other cpp libraries which one might want to print, but we don't need them in the yt project (allegedly). Some of these dependencies are located in [library/cpp/yt/string/format_extensions]. If you don't care about granularity, simply include "library/cpp/yt/string/format_extensions/all.h" to enable support for every currently known dependency.
