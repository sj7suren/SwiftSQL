// AiConfigStore.h — persists ai::AiSettings (the list of providers + the default
// selection + the privacy gate) to the shared settings.ini under the user's data
// dir. Each provider's apiKey is encrypted at rest via core::Secret (AES-GCM on
// Windows, with the AES master key protected by DPAPI). Mirrors that store's shape:
// a plain-struct model (AiConfig.h) + a stateless Load/Save facade here.
#pragma once

#include "ai/AiConfig.h"

namespace ai {

class AiConfigStore {
public:
    // Read the whole AI settings block. apiKeys are decrypted back to plaintext.
    // On first run (nothing persisted yet) returns an empty settings object; call
    // sites typically seed it with BuiltinProviderPresets() when providers is empty.
    static AiSettings Load();

    // Persist the whole block, replacing any previously stored providers. Each
    // apiKey is run through core::EncryptSecret first — plaintext never hits disk.
    static void Save(const AiSettings& settings);
};

} // namespace ai
