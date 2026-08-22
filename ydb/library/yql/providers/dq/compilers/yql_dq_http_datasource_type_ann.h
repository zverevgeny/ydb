#pragma once

#include <yql/essentials/core/transformer/transformer.h>

namespace NYql {

    ///
    /// Register type annotation handler for HTTP lookup source settings node.
    /// Validates node children and assigns output type:
    /// Struct<StatusCode: Uint32, Headers: Dict<String,String>, Body: String>
    ///
    void RegisterDqHttpTypeAnnotation(IGraphTransformer::TAddHandlers& addHandlers);

} // namespace NYql
