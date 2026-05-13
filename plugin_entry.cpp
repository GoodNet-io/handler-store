// SPDX-License-Identifier: Apache-2.0
/// @file   plugins/handlers/store/plugin_entry.cpp
/// @brief  Plugin entry collapsed to `GN_HANDLER_PLUGIN`. Same shape
///         as the heartbeat handler — the macro generates the five
///         `gn_plugin_*` C entry points, builds the handler vtable
///         from `StoreHandler`'s static metadata, and registers the
///         `gn.store` extension via the class's
///         `extension_name` / `extension_version` / `extension_vtable`
///         triplet.

#include <sdk/cpp/handler_plugin.hpp>

#include "store.hpp"

GN_HANDLER_PLUGIN(
    ::gn::handler::store::StoreHandler,
    "goodnet_handler_store",
    "1.0.0-rc1")
