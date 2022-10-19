/*
 * TenantCache.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "fdbclient/FDBTypes.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/Tenant.h"
#include "fdbserver/TCInfo.h"
#include "flow/IRandom.h"
#include "flow/IndexedSet.h"
#include "flow/flow.h"
#include <limits>
#include <string>

typedef Map<KeyRef, Reference<TCTenantInfo>> TenantMapByPrefix;

struct TenantCacheTenantCreated {
	KeyRange keys;
	Promise<bool> reply;
	TenantCacheTenantCreated(Key prefix) { keys = prefixRange(prefix); }
};

class TenantCache : public ReferenceCounted<TenantCache> {
	friend class TenantCacheImpl;
	friend class TenantCacheUnitTest;

private:
	constexpr static uint64_t INVALID_GENERATION = std::numeric_limits<uint64_t>::max();

	UID distributorID;
	Database cx;
	uint64_t generation;
	TenantMapByPrefix tenantCache;

	// mark the start of a new sweep of the tenant cache
	void startRefresh();

	void insert(TenantName& tenantName, TenantMapEntry& tenant);
	void keep(TenantName& tenantName, TenantMapEntry& tenant);

	// return true if a new tenant is inserted into the cache
	bool update(TenantName& tenantName, TenantMapEntry& tenant);

	// return count of tenants that were found to be stale and removed from the cache
	int cleanup();

	// return the mapping from prefix -> tenant name for all tenants stored in the cache
	std::vector<std::pair<KeyRef, TenantName>> getTenantList() const;

	// update the size for a tenant; do nothing if the tenant doesn't exist in the map
	void updateStorageUsage(KeyRef prefix, int64_t size);

	UID id() const { return distributorID; }

	Database dbcx() const { return cx; }

public:
	TenantCache(Database cx, UID distributorID) : distributorID(distributorID), cx(cx) {
		generation = deterministicRandom()->randomUInt32();
	}

	PromiseStream<TenantCacheTenantCreated> tenantCreationSignal;

	Future<Void> build();

	Future<Void> monitorTenantMap();

	Future<Void> monitorStorageUsage();

	std::string desc() const;

	bool isTenantKey(KeyRef key) const;

	Optional<Reference<TCTenantInfo>> tenantOwning(KeyRef key) const;
};
