// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// ConnectionClone.cpp — see header.
#include "db/ConnectionClone.h"

namespace db {

std::unique_ptr<IConnection> CloneConnection(const IConnection& src, wxString& err)
{
    if (!src.IsConnected()) {
        err = L"源连接未建立，无法克隆";
        return nullptr;
    }

    const core::ConnectionProfile& eff = src.EffectiveProfile();
    std::unique_ptr<IConnection> clone = CreateConnection(eff.type);
    if (!clone) {
        err = L"不支持的数据库类型，无法克隆连接";
        return nullptr;
    }
    if (!clone->Connect(eff, err)) return nullptr;   // Connect() fills err on failure
    return clone;
}

} // namespace db
