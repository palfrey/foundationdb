/*
 * DataDistribution.actor.h
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

#if defined(NO_INTELLISENSE) && !defined(FDBSERVER_DATA_DISTRIBUTION_ACTOR_G_H)
#define FDBSERVER_DATA_DISTRIBUTION_ACTOR_G_H
#include "fdbserver/DataDistribution.actor.g.h"
#elif !defined(FDBSERVER_DATA_DISTRIBUTION_ACTOR_H)
#define FDBSERVER_DATA_DISTRIBUTION_ACTOR_H

#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/RunTransaction.actor.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/LogSystem.h"
#include "fdbserver/MoveKeys.actor.h"
#include <boost/heap/policies.hpp>
#include <boost/heap/skew_heap.hpp>

#include "flow/actorcompiler.h" // This must be the last #include.

enum class RelocateReason { INVALID = -1, OTHER, REBALANCE_DISK, REBALANCE_READ, REBALANCE_ROCKSDB_COLUMN };
// REBALANCE_ROCKSDB_COLUMN: rebalance size of physicalShard

struct DDShardInfo;

struct DataMove {
	DataMove() : meta(DataMoveMetaData()), restore(false), valid(false), cancelled(false) {}
	explicit DataMove(DataMoveMetaData meta, bool restore)
	  : meta(std::move(meta)), restore(restore), valid(true), cancelled(meta.getPhase() == DataMoveMetaData::Deleting) {
	}

	// Checks if the DataMove is consistent with the shard.
	void validateShard(const DDShardInfo& shard, KeyRangeRef range, int priority = SERVER_KNOBS->PRIORITY_RECOVER_MOVE);

	bool isCancelled() const { return this->cancelled; }

	const DataMoveMetaData meta;
	bool restore;
	bool valid;
	bool cancelled;
	std::vector<UID> primarySrc;
	std::vector<UID> remoteSrc;
	std::vector<UID> primaryDest;
	std::vector<UID> remoteDest;
};

struct RelocateShard {
	KeyRange keys;
	int priority;
	bool cancelled;
	std::shared_ptr<DataMove> dataMove;
	UID dataMoveId;
	RelocateReason reason;
	RelocateShard() : priority(0), cancelled(false), reason(RelocateReason::INVALID) {}
	RelocateShard(KeyRange const& keys, int priority, RelocateReason reason)
	  : keys(keys), priority(priority), cancelled(false), reason(reason) {}

	bool isRestore() const { return this->dataMove != nullptr; }
};

struct IDataDistributionTeam {
	virtual std::vector<StorageServerInterface> getLastKnownServerInterfaces() const = 0;
	virtual int size() const = 0;
	virtual std::vector<UID> const& getServerIDs() const = 0;
	virtual void addDataInFlightToTeam(int64_t delta) = 0;
	virtual void addReadInFlightToTeam(int64_t delta) = 0;
	virtual int64_t getDataInFlightToTeam() const = 0;
	virtual int64_t getLoadBytes(bool includeInFlight = true, double inflightPenalty = 1.0) const = 0;
	virtual int64_t getReadInFlightToTeam() const = 0;
	virtual double getLoadReadBandwidth(bool includeInFlight = true, double inflightPenalty = 1.0) const = 0;
	virtual int64_t getMinAvailableSpace(bool includeInFlight = true) const = 0;
	virtual double getMinAvailableSpaceRatio(bool includeInFlight = true) const = 0;
	virtual bool hasHealthyAvailableSpace(double minRatio) const = 0;
	virtual Future<Void> updateStorageMetrics() = 0;
	virtual void addref() const = 0;
	virtual void delref() const = 0;
	virtual bool isHealthy() const = 0;
	virtual void setHealthy(bool) = 0;
	virtual int getPriority() const = 0;
	virtual void setPriority(int) = 0;
	virtual bool isOptimal() const = 0;
	virtual bool isWrongConfiguration() const = 0;
	virtual void setWrongConfiguration(bool) = 0;
	virtual void addServers(const std::vector<UID>& servers) = 0;
	virtual std::string getTeamID() const = 0;

	std::string getDesc() const {
		const auto& servers = getLastKnownServerInterfaces();
		std::string s = format("TeamID %s; ", getTeamID().c_str());
		s += format("Size %d; ", servers.size());
		for (int i = 0; i < servers.size(); i++) {
			if (i)
				s += ", ";
			s += servers[i].address().toString() + " " + servers[i].id().shortString();
		}
		return s;
	}
};

FDB_DECLARE_BOOLEAN_PARAM(WantNewServers);
FDB_DECLARE_BOOLEAN_PARAM(WantTrueBest);
FDB_DECLARE_BOOLEAN_PARAM(PreferLowerDiskUtil);
FDB_DECLARE_BOOLEAN_PARAM(TeamMustHaveShards);
FDB_DECLARE_BOOLEAN_PARAM(ForReadBalance);
FDB_DECLARE_BOOLEAN_PARAM(PreferLowerReadUtil);

struct GetTeamRequest {
	bool wantsNewServers; // In additional to servers in completeSources, try to find teams with new server
	bool wantsTrueBest;
	bool preferLowerDiskUtil; // if true, lower utilized team has higher score
	bool teamMustHaveShards;
	bool forReadBalance;
	bool preferLowerReadUtil; // only make sense when forReadBalance is true
	double inflightPenalty;
	bool findTeamByServers;
	std::vector<UID> completeSources;
	std::vector<UID> src;
	Promise<std::pair<Optional<Reference<IDataDistributionTeam>>, bool>> reply;

	typedef Reference<IDataDistributionTeam> TeamRef;

	GetTeamRequest() {}
	GetTeamRequest(WantNewServers wantsNewServers,
	               WantTrueBest wantsTrueBest,
	               PreferLowerDiskUtil preferLowerDiskUtil,
	               TeamMustHaveShards teamMustHaveShards,
	               ForReadBalance forReadBalance = ForReadBalance::False,
	               PreferLowerReadUtil preferLowerReadUtil = PreferLowerReadUtil::False,
	               double inflightPenalty = 1.0)
	  : wantsNewServers(wantsNewServers), wantsTrueBest(wantsTrueBest), preferLowerDiskUtil(preferLowerDiskUtil),
	    teamMustHaveShards(teamMustHaveShards), forReadBalance(forReadBalance),
	    preferLowerReadUtil(preferLowerReadUtil), inflightPenalty(inflightPenalty), findTeamByServers(false) {}
	GetTeamRequest(std::vector<UID> servers)
	  : wantsNewServers(wantsNewServers), wantsTrueBest(wantsTrueBest), preferLowerDiskUtil(PreferLowerDiskUtil::False),
	    teamMustHaveShards(teamMustHaveShards), forReadBalance(forReadBalance),
	    preferLowerReadUtil(preferLowerReadUtil), inflightPenalty(inflightPenalty), findTeamByServers(true),
	    src(std::move(servers)) {}

	// return true if a.score < b.score
	[[nodiscard]] bool lessCompare(TeamRef a, TeamRef b, int64_t aLoadBytes, int64_t bLoadBytes) const {
		int res = 0;
		if (forReadBalance) {
			res = preferLowerReadUtil ? greaterReadLoad(a, b) : lessReadLoad(a, b);
		}
		return res == 0 ? lessCompareByLoad(aLoadBytes, bLoadBytes) : res < 0;
	}

	std::string getDesc() const {
		std::stringstream ss;

		ss << "WantsNewServers:" << wantsNewServers << " WantsTrueBest:" << wantsTrueBest
		   << " PreferLowerDiskUtil:" << preferLowerDiskUtil << " teamMustHaveShards:" << teamMustHaveShards
		   << "forReadBalance" << forReadBalance << " inflightPenalty:" << inflightPenalty << ";";
		ss << "CompleteSources:";
		for (const auto& cs : completeSources) {
			ss << cs.toString() << ",";
		}

		return std::move(ss).str();
	}

private:
	// return true if preferHigherUtil && aLoadBytes <= bLoadBytes (higher load bytes has larger score)
	// or preferLowerUtil && aLoadBytes > bLoadBytes
	bool lessCompareByLoad(int64_t aLoadBytes, int64_t bLoadBytes) const {
		bool lessLoad = aLoadBytes <= bLoadBytes;
		return preferLowerDiskUtil ? !lessLoad : lessLoad;
	}

	// return -1 if a.readload > b.readload
	static int greaterReadLoad(TeamRef a, TeamRef b) {
		auto r1 = a->getLoadReadBandwidth(true), r2 = b->getLoadReadBandwidth(true);
		return r1 == r2 ? 0 : (r1 > r2 ? -1 : 1);
	}
	// return -1 if a.readload < b.readload
	static int lessReadLoad(TeamRef a, TeamRef b) {
		auto r1 = a->getLoadReadBandwidth(false), r2 = b->getLoadReadBandwidth(false);
		return r1 == r2 ? 0 : (r1 < r2 ? -1 : 1);
	}
};

struct GetMetricsRequest {
	KeyRange keys;
	Promise<StorageMetrics> reply;
	GetMetricsRequest() {}
	GetMetricsRequest(KeyRange const& keys) : keys(keys) {}
};

struct GetTopKMetricsReply {
	std::vector<StorageMetrics> metrics;
	double minReadLoad = -1, maxReadLoad = -1;
	GetTopKMetricsReply() {}
	GetTopKMetricsReply(std::vector<StorageMetrics> const& m, double minReadLoad, double maxReadLoad)
	  : metrics(m), minReadLoad(minReadLoad), maxReadLoad(maxReadLoad) {}
};
struct GetTopKMetricsRequest {
	// whether a > b
	typedef std::function<bool(const StorageMetrics& a, const StorageMetrics& b)> MetricsComparator;
	int topK = 1; // default only return the top 1 shard based on the comparator
	MetricsComparator comparator; // Return true if a.score > b.score, return the largest topK in keys
	std::vector<KeyRange> keys;
	Promise<GetTopKMetricsReply> reply; // topK storage metrics
	double maxBytesReadPerKSecond = 0, minBytesReadPerKSecond = 0; // all returned shards won't exceed this read load

	GetTopKMetricsRequest() {}
	GetTopKMetricsRequest(std::vector<KeyRange> const& keys,
	                      int topK = 1,
	                      double maxBytesReadPerKSecond = std::numeric_limits<double>::max(),
	                      double minBytesReadPerKSecond = 0)
	  : topK(topK), keys(keys), maxBytesReadPerKSecond(maxBytesReadPerKSecond),
	    minBytesReadPerKSecond(minBytesReadPerKSecond) {}
};

struct GetMetricsListRequest {
	KeyRange keys;
	int shardLimit;
	Promise<Standalone<VectorRef<DDMetricsRef>>> reply;

	GetMetricsListRequest() {}
	GetMetricsListRequest(KeyRange const& keys, const int shardLimit) : keys(keys), shardLimit(shardLimit) {}
};

class ShardsAffectedByTeamFailure;
struct StorageServerMetric {
	StorageMetrics metrics;
	int64_t bytesLag;
	int64_t versionLag;
	double cpuUsage;
	double diskUsage;
	double localRateLimit;
};

struct TeamMetrics {
	std::vector<std::pair<UID, Optional<GetStorageMetricsReply>>> ssMetricsList;
	std::string toString() const {
		std::string result = "";
		for (auto& ssMetrics : ssMetricsList) {
			if (ssMetrics.second.present()) {
				result = result + ssMetrics.first.toString() + "/" + std::to_string(ssMetrics.second.get().versionLag) +
				         "/" + std::to_string(ssMetrics.second.get().bytesInputRate) + ";";
			} else {
				result = result + ssMetrics.first.toString() + "-NONE;";
			}
		}
		return result;
	}
};

using TeamAndMetricTuple = std::tuple<Reference<IDataDistributionTeam>, bool, TeamMetrics>;
struct TeamsAndMetrics {
	std::vector<TeamAndMetricTuple> teams;
};

struct GetStorageServerStatusRequest {
	UID ssid;
	Promise<StorageServerMetric> reply;
	GetStorageServerStatusRequest(UID ssid) : ssid(ssid) {}
};

struct GetTeamStatusRequest {
	std::vector<UID> servers;
	Promise<TeamMetrics> reply;
	GetTeamStatusRequest(std::vector<UID> servers) : servers(servers) {}
};

struct GetTeamsAndMetricsRequest {
	int teamCounts;
	Promise<TeamsAndMetrics> reply;
	std::vector<std::vector<UID>> teams;
	bool findTeamByServers;

	GetTeamsAndMetricsRequest() : teamCounts(SERVER_KNOBS->TEAM_COUNT_TAKEN_BY_GET_TEAMS), findTeamByServers(false) {}
	GetTeamsAndMetricsRequest(std::vector<std::vector<UID>> teams) : findTeamByServers(true), teams(teams) {}
};

struct TeamCollectionInterface {
	PromiseStream<GetTeamRequest> getTeam;
	PromiseStream<GetStorageServerStatusRequest> getStorageServerStatus;
	PromiseStream<GetTeamStatusRequest> getTeamStatus;
	PromiseStream<GetTeamsAndMetricsRequest> getTeamsAndMetrics;
};

class ShardsAffectedByTeamFailure : public ReferenceCounted<ShardsAffectedByTeamFailure> {
public:
	ShardsAffectedByTeamFailure() {}

	struct Team {
		std::vector<UID> servers; // sorted
		bool primary;

		Team() : primary(true) {}
		Team(std::vector<UID> const& servers, bool primary) : servers(servers), primary(primary) {}

		bool operator<(const Team& r) const {
			if (servers == r.servers)
				return primary < r.primary;
			return servers < r.servers;
		}
		bool operator>(const Team& r) const { return r < *this; }
		bool operator<=(const Team& r) const { return !(*this > r); }
		bool operator>=(const Team& r) const { return !(*this < r); }
		bool operator==(const Team& r) const { return servers == r.servers && primary == r.primary; }
		bool operator!=(const Team& r) const { return !(*this == r); }

		std::string toString() const { return describe(servers); };
	};

	// This tracks the data distribution on the data distribution server so that teamTrackers can
	//   relocate the right shards when a team is degraded.

	// The following are important to make sure that failure responses don't revert splits or merges:
	//   - The shards boundaries in the two data structures reflect "queued" RelocateShard requests
	//       (i.e. reflects the desired set of shards being tracked by dataDistributionTracker,
	//       rather than the status quo).  These boundaries are modified in defineShard and the content
	//       of what servers correspond to each shard is a copy or union of the shards already there
	//   - The teams associated with each shard reflect either the sources for non-moving shards
	//       or the destination team for in-flight shards (the change is atomic with respect to team selection).
	//       moveShard() changes the servers associated with a shard and will never adjust the shard
	//       boundaries. If a move is received for a shard that has been redefined (the exact shard is
	//       no longer in the map), the servers will be set for all contained shards and added to all
	//       intersecting shards.

	int getNumberOfShards(UID ssID) const;
	std::vector<KeyRange> getShardsFor(Team team) const;
	bool hasShards(Team team) const;

	// The first element of the pair is either the source for non-moving shards or the destination team for in-flight
	// shards The second element of the pair is all previous sources for in-flight shards
	std::pair<std::vector<Team>, std::vector<Team>> getTeamsFor(KeyRangeRef keys);

	void defineShard(KeyRangeRef keys);
	void moveShard(KeyRangeRef keys, std::vector<Team> destinationTeam);
	void finishMove(KeyRangeRef keys);
	void check() const;

	PromiseStream<KeyRange> restartShardTracker;

private:
	struct OrderByTeamKey {
		bool operator()(const std::pair<Team, KeyRange>& lhs, const std::pair<Team, KeyRange>& rhs) const {
			if (lhs.first < rhs.first)
				return true;
			if (lhs.first > rhs.first)
				return false;
			return lhs.second.begin < rhs.second.begin;
		}
	};

	KeyRangeMap<std::pair<std::vector<Team>, std::vector<Team>>>
	    shard_teams; // A shard can be affected by the failure of multiple teams if it is a queued merge, or when
	                 // usable_regions > 1
	std::set<std::pair<Team, KeyRange>, OrderByTeamKey> team_shards;
	std::map<UID, int> storageServerShards;

	void erase(Team team, KeyRange const& range);
	void insert(Team team, KeyRange const& range);
};

class DDTeamCollection;

class PhysicalShardCollection : public ReferenceCounted<PhysicalShardCollection> {
public:
	struct PhysicalShard {
		uint64_t id;
		StorageMetrics metrics;

		PhysicalShard() : id(0) {}
		explicit PhysicalShard(uint64_t id) : id(id), metrics(StorageMetrics()) {
			ASSERT(id != UID().first());
			ASSERT(id != anonymousShardId.first());
		}
		explicit PhysicalShard(uint64_t id, StorageMetrics const& metrics) : id(id), metrics(metrics) {
			ASSERT(id != UID().first());
			ASSERT(id != anonymousShardId.first());
		}
		// operator< used for selecting the physicalShard with the minimal bytesOnDisk
		bool operator<(const struct PhysicalShard& right) const { return id < right.id ? true : false; }
		std::string toString() const { return std::to_string(id); }
	};

	// PhysicalShard Core
	// the mapping from a physicalShardID to its corresponding physicalShard
	std::map<uint64_t, PhysicalShard> physicalShardCollection;
	// the mapping from keyRange to physicalShardIDs
	KeyRangeMap<uint64_t> keyRangePhysicalShardIDMap;
	// the mapping from a team to physicalShards of the team
	std::map<ShardsAffectedByTeamFailure::Team, std::set<uint64_t>> teamPhysicalShardIDs;

	// maintain the mapping between teams and physicalShards
	void updatePhysicalShardToTeams(uint64_t physicalShardID,
	                                std::vector<ShardsAffectedByTeamFailure::Team> inputTeams,
	                                int expectedNumServersPerTeam,
	                                uint64_t debugID);
	Optional<uint64_t> trySelectPhysicalShardFor(ShardsAffectedByTeamFailure::Team team,
	                                             StorageMetrics const& metrics,
	                                             uint64_t debugID);
	bool checkPhysicalShardValid(uint64_t physicalShardID, StorageMetrics const& moveInMetrics);
	Optional<ShardsAffectedByTeamFailure::Team> tryGetValidRemoteTeamWith(uint64_t physicalShardID,
	                                                                      StorageMetrics const& moveInMetrics,
	                                                                      int expectedTeamSize,
	                                                                      uint64_t debugID);
	std::vector<PhysicalShard> getValidPhysicalShardsOf(ShardsAffectedByTeamFailure::Team team,
	                                                    StorageMetrics const& moveInMetrics,
	                                                    uint64_t debugID);
	std::vector<ShardsAffectedByTeamFailure::Team> getValidPairedRemoteTeamsOf(ShardsAffectedByTeamFailure::Team team,
	                                                                           StorageMetrics const& moveInMetrics,
	                                                                           int expectedTeamSize,
	                                                                           uint64_t debugID);
	uint64_t generateNewPhysicalShardID(uint64_t debugID);

	// PhysicalShard Metrics
	std::vector<uint64_t> updatePhysicalShardMetricsByKeyRange(KeyRange keys,
	                                                           StorageMetrics const& newMetrics,
	                                                           StorageMetrics const& oldMetrics,
	                                                           bool initWithNewMetrics);
	void reduceMetricsForMoveOut(uint64_t physicalShardID, StorageMetrics const& metrics);
	void increaseMetricsForMoveIn(uint64_t physicalShardID, StorageMetrics const& metrics);

	// to remove
	void printTeamPhysicalShardsMapping(std::string);
};

enum DataMoveType { PHYSICAL_SHARD_MOVE, READ_RANGE_MOVE };
class DDEventBuffer : public ReferenceCounted<DDEventBuffer> {
public:
	DDEventBuffer() {}
	struct DDEvent {
		// which event type?
		int eventType; // equivalent to rs.priority
		// how to move (suggested)?
		Optional<DataMoveType> dataMoveType;
		// who triggers the event
		Optional<KeyRange> keyRange;
		Optional<uint64_t> physicalShard;
		Optional<UID> storageServer;
		Optional<ShardsAffectedByTeamFailure::Team> team;
		// any RelocateShard suggested?
		Optional<RelocateShard> rs;
		DDEvent(int eventType) : eventType(eventType) {}
		DDEvent(int eventType, uint64_t physicalShardID) : eventType(eventType), physicalShard(physicalShardID) {}
		DDEvent(int eventType, KeyRange keyRange) : eventType(eventType), keyRange(keyRange) {}
		DDEvent(int eventType, RelocateShard rs) : eventType(eventType), rs(rs) {}
		std::string toString() const { return std::to_string(eventType); }
	};
	void append(DDEvent event) { buffer.push_back(event); }
	std::vector<DDEvent> takeAll() {
		std::vector<DDEvent> result(buffer);
		buffer.clear();
		return result;
	}
	bool empty() { return (buffer.size() == 0); }

private:
	std::vector<DDEvent> buffer;
};

class DataDistributionRuntimeMonitor : public ReferenceCounted<DataDistributionRuntimeMonitor> {
public:
	DataDistributionRuntimeMonitor() {}
	// DD Algorithm Support: Runtime Metrics
	TeamMetrics getTeamMetrics(ShardsAffectedByTeamFailure::Team team);
	StorageServerMetric getStorageServerMetrics(UID serverID);
	StorageMetrics getPhysicalShardMetrics(uint64_t physicalShardID);
	StorageMetrics getKeyRangeMetrics(KeyRange keyRange);
	// DD Algorithm Support: Issue Data Move
	void issuePhysicalShardMove(uint64_t physicalShardID, Optional<std::vector<KeyRange>> keyRanges);
	void issueReadRangeMove(KeyRange keyRange);
	// DD Init
	void setTeamCollections(std::vector<TeamCollectionInterface> tcs) { teamCollections = tcs; }
	void setGetShardMetrics(PromiseStream<GetMetricsRequest> getMetrics) { getShardMetrics = getMetrics; }
	void setPhysicalShardCollection(Reference<PhysicalShardCollection> collection) {
		physicalShardCollection = collection;
	}
	void setRelocateBuffer(PromiseStream<RelocateShard> buffer) { relocateBuffer = buffer; }
	void setDDEventBuffer(Reference<DDEventBuffer> buffer) { ddEventBuffer = buffer; };

	void triggerDDEvent(DDEventBuffer::DDEvent inputEvent, bool immediate) {
		ASSERT(CLIENT_KNOBS->PHYSICAL_SHARD_AWARE_DD);

		ddEventBuffer->append(inputEvent);
		if (!immediate) {
			return;
		}

		TraceEvent e("TriggerDataMove");
		std::vector<DDEventBuffer::DDEvent> events = ddEventBuffer->takeAll();
		e.detail("Events", events);
		for (auto event : events) {
			if (event.rs.present()) {
				relocateBuffer.send(event.rs.get());
				continue;
			}
			// PhysicalShard is too large or too (small and cold)
			ASSERT(CLIENT_KNOBS->PHYSICAL_SHARD_SIZE_CONTROL);
			ASSERT(event.physicalShard.present());
			if (event.eventType == SERVER_KNOBS->PRIORITY_SPLIT_PHYSICAL_SHARD) {
				// move out half physicalShards
				uint64_t physicalShardID = event.physicalShard.get();
				std::vector<KeyRange> keyRanges;
				KeyRangeMap<uint64_t>::Ranges keyRangePhysicalShardIDRanges =
				    physicalShardCollection->keyRangePhysicalShardIDMap.ranges();
				KeyRangeMap<uint64_t>::iterator it = keyRangePhysicalShardIDRanges.begin();
				int numKeyRanges = 0;
				for (; it != keyRangePhysicalShardIDRanges.end(); ++it) {
					if (physicalShardID == it->value()) {
						KeyRange keyRange = Standalone(KeyRangeRef(it->range().begin, it->range().end));
						keyRanges.push_back(keyRange);
						numKeyRanges = numKeyRanges + 1;
					}
				}
				int counter = 0;
				for (auto& keyRange : keyRanges) {
					if (counter > numKeyRanges / 2) {
						break;
					}
					relocateBuffer.send(
					    RelocateShard(keyRange, event.eventType, RelocateReason::REBALANCE_ROCKSDB_COLUMN));
					counter = counter + 1;
				}
			} else if (event.eventType == SERVER_KNOBS->PRIORITY_MERGE_PHYSICAL_SHARD) {
				// TODO: at this point we know which physicalShard is too small
				continue;
			} else {
				UNREACHABLE();
			}
		}
		return;
	}

	struct PhysicalShardAwareBestTeams {
		uint64_t physicalShardID;
		std::vector<std::pair<Reference<IDataDistributionTeam>, bool>> bestTeams;
	};

	using PhysicalShardAwareTeamStats =
	    std::map<uint64_t, std::pair<PhysicalShardCollection::PhysicalShard, std::vector<TeamAndMetricTuple>>>;

	Optional<PhysicalShardAwareBestTeams> selectTeamsAndPhysicalShard(PhysicalShardAwareTeamStats teamStats,
	                                                                  int numDC,
	                                                                  uint64_t debugID) {
		ASSERT(CLIENT_KNOBS->PHYSICAL_SHARD_AWARE_GET_TEAM);
		ASSERT(teamStats.size() > 0);

		int64_t maxPhysicalShardBytes = 0;
		int64_t minPhysicalShardBytes = StorageMetrics::infinity;
		int64_t maxMaxLag = 0;
		int64_t minMaxLag = StorageMetrics::infinity;

		// std::cout << "SelectCandidates\n";
		TraceEvent e("SelectCandidates");
		e.detail("DebugID", debugID);
		for (auto& [physicalShardID, stats] : teamStats) {
			ASSERT(stats.second.size() == numDC);
			int64_t physicalShardBytes = stats.first.metrics.bytes;
			// std::cout << "PhysicalShard: " << physicalShardID << physicalShardBytes << "\n";
			for (auto& teamAndMetric : stats.second) {
				// std::cout << "Team: " <<  serversToString(std::get<0>(teamAndMetric)->getServerIDs()) << "\n";
				// std::cout << "Metric: " << describe(std::get<2>(teamAndMetric)) << "\n";
				int64_t maxLag = getMaxVerLag(std::get<2>(teamAndMetric));
				if (maxLag == -1) {
					continue;
				}
				maxMaxLag = std::max(maxMaxLag, maxLag);
				minMaxLag = std::min(minMaxLag, maxLag);
			}
			// std::cout << "--------------------------------------------------------------\n";
			maxPhysicalShardBytes = std::max(maxPhysicalShardBytes, physicalShardBytes);
			minPhysicalShardBytes = std::min(minPhysicalShardBytes, physicalShardBytes);
		}
		e.detail("MaxPhysicalShardBytes", maxPhysicalShardBytes);
		e.detail("MinPhysicalShardBytes", minPhysicalShardBytes);
		e.detail("MaxMaxLag", maxMaxLag);
		e.detail("MinMaxLag", minMaxLag);

		// std::cout << "PHBytes: " << minPhysicalShardBytes << "~" << maxPhysicalShardBytes << "; Lag: " << minMaxLag
		// << "~" << maxMaxLag << "\n";

		if (maxPhysicalShardBytes == 0 || minPhysicalShardBytes == StorageMetrics::infinity || maxMaxLag == 0 ||
		    minMaxLag == StorageMetrics::infinity) {
			// std::cout << "return 1\n";
			return Optional<PhysicalShardAwareBestTeams>();
		}

		double bestScore = 0;
		uint64_t bestPhysicalShardID = UID().first();
		int64_t bestLag = 0;
		int64_t bestPHBytes = 0;
		for (auto& [physicalShardID, stats] : teamStats) {
			int64_t physicalShardBytes = stats.first.metrics.bytes;
			double score = 0;
			score = score + (maxPhysicalShardBytes - physicalShardBytes + 1) * 1.0 /
			                    (maxPhysicalShardBytes - minPhysicalShardBytes + 1);
			// std::cout << "(" << maxPhysicalShardBytes << "-" << physicalShardBytes << ")" << "/" << "(" <<
			// maxPhysicalShardBytes << "-" << minPhysicalShardBytes << ")\n";
			int64_t maxLag = 0;
			bool missSSMetric = false;
			for (auto& teamAndMetric : stats.second) {
				int64_t tmp = getMaxVerLag(std::get<2>(teamAndMetric));
				if (tmp == -1) {
					// std::cout << "missSSMetric? tmp=" << tmp << "\n";
					missSSMetric = true;
					break;
				}
				maxLag = std::max(maxLag, tmp);
			}
			if (missSSMetric) {
				continue;
			}
			score = score + (maxMaxLag - maxLag + 1) * 1.0 / (maxMaxLag - minMaxLag + 1);
			// std::cout << "(" << maxMaxLag << "-" << maxLag << ")" << "/" << "(" << maxMaxLag << "-" << minMaxLag <<
			// ")\n"; std::cout << "Score: " << score << "\n";
			if (score > bestScore) {
				bestPhysicalShardID = physicalShardID;
				bestScore = score;
				bestLag = maxLag;
				bestPHBytes = physicalShardBytes;
			}
		}

		if (bestPhysicalShardID == UID().first()) {
			// std::cout << "return 2? size=" << teamStats.size() << "\n";
			return Optional<PhysicalShardAwareBestTeams>();
		}
		ASSERT(bestPhysicalShardID != anonymousShardId.first());
		PhysicalShardAwareBestTeams res;
		res.physicalShardID = bestPhysicalShardID;
		e.detail("BestPhysicalShardID", bestPhysicalShardID);
		e.detail("MaxLag", bestLag);
		e.detail("PhysicalShardBytes", bestPHBytes);
		ASSERT(teamStats[bestPhysicalShardID].second.size() == 1 || teamStats[bestPhysicalShardID].second.size() == 2);
		for (auto& teamAndMetric : teamStats[bestPhysicalShardID].second) {
			res.bestTeams.push_back(std::make_pair(std::get<0>(teamAndMetric), std::get<1>(teamAndMetric)));
		}
		std::cout << "BestPhysicalShardID: " << bestPhysicalShardID << "\n";
		std::cout << "MaxLag: " << bestLag << "\n";
		std::cout << "PhysicalShardBytes: " << bestPHBytes << "\n\n\n\n\n\n";
		return res;
	}

private:
	// DD Algorithm Support: Issue Data Move
	// DataDistributionRuntimeMonitor takes ddEventBuffer as input and puts outputs to relocateBuffer
	Reference<DDEventBuffer> ddEventBuffer;
	PromiseStream<RelocateShard> relocateBuffer; // self->output.send(RelocateShard)

	// DD Algorithm Support: Runtime Metrics
	std::vector<TeamCollectionInterface> teamCollections; // get team/storageServer metrics
	Reference<PhysicalShardCollection> physicalShardCollection; // get physicalShard metrics
	PromiseStream<GetMetricsRequest> getShardMetrics; // get keyRange metrics

	std::string serversToString(std::vector<UID> servers) {
		ASSERT(CLIENT_KNOBS->PHYSICAL_SHARD_AWARE_GET_TEAM);

		ASSERT(!servers.empty());
		std::sort(servers.begin(), servers.end());
		std::stringstream ss;
		for (const auto& id : servers) {
			ss << id.toString() << " ";
		}
		return ss.str();
	}

	int64_t getMaxVerLag(TeamMetrics teamMetrics) {
		int64_t maxLag = -1;
		for (auto& ssMetrics : teamMetrics.ssMetricsList) {
			if (ssMetrics.second.present()) {
				maxLag = std::max(maxLag, ssMetrics.second.get().versionLag);
			}
		}
		return maxLag;
	}
};

// DDShardInfo is so named to avoid link-time name collision with ShardInfo within the StorageServer
struct DDShardInfo {
	Key key;
	std::vector<UID> primarySrc;
	std::vector<UID> remoteSrc;
	std::vector<UID> primaryDest;
	std::vector<UID> remoteDest;
	bool hasDest;
	UID srcId;
	UID destId;

	explicit DDShardInfo(Key key) : key(key), hasDest(false) {}
	DDShardInfo(Key key, UID srcId, UID destId) : key(key), hasDest(false), srcId(srcId), destId(destId) {}
};

struct InitialDataDistribution : ReferenceCounted<InitialDataDistribution> {
	InitialDataDistribution() : dataMoveMap(std::make_shared<DataMove>()) {}

	int mode;
	std::vector<std::pair<StorageServerInterface, ProcessClass>> allServers;
	std::set<std::vector<UID>> primaryTeams;
	std::set<std::vector<UID>> remoteTeams;
	std::vector<DDShardInfo> shards;
	Optional<Key> initHealthyZoneValue;
	KeyRangeMap<std::shared_ptr<DataMove>> dataMoveMap;
};

struct ShardMetrics {
	StorageMetrics metrics;
	double lastLowBandwidthStartTime;
	int shardCount; // number of smaller shards whose metrics are aggregated in the ShardMetrics

	bool operator==(ShardMetrics const& rhs) const {
		return metrics == rhs.metrics && lastLowBandwidthStartTime == rhs.lastLowBandwidthStartTime &&
		       shardCount == rhs.shardCount;
	}

	ShardMetrics(StorageMetrics const& metrics, double lastLowBandwidthStartTime, int shardCount)
	  : metrics(metrics), lastLowBandwidthStartTime(lastLowBandwidthStartTime), shardCount(shardCount) {}
};

struct ShardTrackedData {
	Future<Void> trackShard;
	Future<Void> trackBytes;
	Reference<AsyncVar<Optional<ShardMetrics>>> stats;
};

ACTOR Future<Void> dataDistributionTracker(Reference<InitialDataDistribution> initData,
                                           Database cx,
                                           PromiseStream<RelocateShard> output,
                                           Reference<ShardsAffectedByTeamFailure> shardsAffectedByTeamFailure,
                                           PromiseStream<GetMetricsRequest> getShardMetrics,
                                           FutureStream<GetTopKMetricsRequest> getTopKMetrics,
                                           PromiseStream<GetMetricsListRequest> getShardMetricsList,
                                           FutureStream<Promise<int64_t>> getAverageShardBytes,
                                           Promise<Void> readyToStart,
                                           Reference<AsyncVar<bool>> zeroHealthyTeams,
                                           UID distributorId,
                                           KeyRangeMap<ShardTrackedData>* shards,
                                           bool* trackerCancelled,
                                           Reference<PhysicalShardCollection> physicalShardCollection,
                                           Reference<DataDistributionRuntimeMonitor> dataDistributionRuntimeMonitor);

ACTOR Future<Void> dataDistributionQueue(Database cx,
                                         Future<Void> readyToStart,
                                         PromiseStream<RelocateShard> output,
                                         FutureStream<RelocateShard> input,
                                         PromiseStream<GetMetricsRequest> getShardMetrics,
                                         PromiseStream<GetTopKMetricsRequest> getTopKMetrics,
                                         Reference<AsyncVar<bool>> processingUnhealthy,
                                         Reference<AsyncVar<bool>> processingWiggle,
                                         std::vector<TeamCollectionInterface> teamCollection,
                                         Reference<ShardsAffectedByTeamFailure> shardsAffectedByTeamFailure,
                                         MoveKeysLock lock,
                                         PromiseStream<Promise<int64_t>> getAverageShardBytes,
                                         FutureStream<Promise<int>> getUnhealthyRelocationCount,
                                         UID distributorId,
                                         int teamSize,
                                         int singleRegionTeamSize,
                                         const DDEnabledState* ddEnabledState,
                                         Reference<PhysicalShardCollection> physicalShardCollection,
                                         Reference<DataDistributionRuntimeMonitor> dataDistributionRuntimeMonitor);

// Holds the permitted size and IO Bounds for a shard
struct ShardSizeBounds {
	StorageMetrics max;
	StorageMetrics min;
	StorageMetrics permittedError;

	bool operator==(ShardSizeBounds const& rhs) const {
		return max == rhs.max && min == rhs.min && permittedError == rhs.permittedError;
	}
};

// Gets the permitted size and IO bounds for a shard
ShardSizeBounds getShardSizeBounds(KeyRangeRef shard, int64_t maxShardSize);

// Determines the maximum shard size based on the size of the database
int64_t getMaxShardSize(double dbSizeEstimate);

struct StorageWiggleMetrics {
	constexpr static FileIdentifier file_identifier = 4728961;

	// round statistics
	// One StorageServer wiggle round is considered 'complete', when all StorageServers with creationTime < T are
	// wiggled
	// Start and finish are in epoch seconds
	double last_round_start = 0;
	double last_round_finish = 0;
	TimerSmoother smoothed_round_duration;
	int finished_round = 0; // finished round since storage wiggle is open

	// step statistics
	// 1 wiggle step as 1 storage server is wiggled in the current round
	// Start and finish are in epoch seconds
	double last_wiggle_start = 0;
	double last_wiggle_finish = 0;
	TimerSmoother smoothed_wiggle_duration;
	int finished_wiggle = 0; // finished step since storage wiggle is open

	StorageWiggleMetrics() : smoothed_round_duration(20.0 * 60), smoothed_wiggle_duration(10.0 * 60) {}

	template <class Ar>
	void serialize(Ar& ar) {
		double step_total, round_total;
		if (!ar.isDeserializing) {
			step_total = smoothed_wiggle_duration.getTotal();
			round_total = smoothed_round_duration.getTotal();
		}
		serializer(ar,
		           last_wiggle_start,
		           last_wiggle_finish,
		           step_total,
		           finished_wiggle,
		           last_round_start,
		           last_round_finish,
		           round_total,
		           finished_round);
		if (ar.isDeserializing) {
			smoothed_round_duration.reset(round_total);
			smoothed_wiggle_duration.reset(step_total);
		}
	}

	static Future<Void> runSetTransaction(Reference<ReadYourWritesTransaction> tr,
	                                      bool primary,
	                                      StorageWiggleMetrics metrics) {
		tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
		tr->setOption(FDBTransactionOptions::LOCK_AWARE);
		tr->set(perpetualStorageWiggleStatsPrefix.withSuffix(primary ? "primary"_sr : "remote"_sr),
		        ObjectWriter::toValue(metrics, IncludeVersion()));
		return Void();
	}

	static Future<Void> runSetTransaction(Database cx, bool primary, StorageWiggleMetrics metrics) {
		return runRYWTransaction(cx, [=](Reference<ReadYourWritesTransaction> tr) -> Future<Void> {
			return runSetTransaction(tr, primary, metrics);
		});
	}

	static Future<Optional<Value>> runGetTransaction(Reference<ReadYourWritesTransaction> tr, bool primary) {
		tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
		tr->setOption(FDBTransactionOptions::READ_LOCK_AWARE);
		return tr->get(perpetualStorageWiggleStatsPrefix.withSuffix(primary ? "primary"_sr : "remote"_sr));
	}

	static Future<Optional<Value>> runGetTransaction(Database cx, bool primary) {
		return runRYWTransaction(cx, [=](Reference<ReadYourWritesTransaction> tr) -> Future<Optional<Value>> {
			return runGetTransaction(tr, primary);
		});
	}

	StatusObject toJSON() const {
		StatusObject result;
		result["last_round_start_datetime"] = epochsToGMTString(last_round_start);
		result["last_round_finish_datetime"] = epochsToGMTString(last_round_finish);
		result["last_round_start_timestamp"] = last_round_start;
		result["last_round_finish_timestamp"] = last_round_finish;
		result["smoothed_round_seconds"] = smoothed_round_duration.smoothTotal();
		result["finished_round"] = finished_round;

		result["last_wiggle_start_datetime"] = epochsToGMTString(last_wiggle_start);
		result["last_wiggle_finish_datetime"] = epochsToGMTString(last_wiggle_finish);
		result["last_wiggle_start_timestamp"] = last_wiggle_start;
		result["last_wiggle_finish_timestamp"] = last_wiggle_finish;
		result["smoothed_wiggle_seconds"] = smoothed_wiggle_duration.smoothTotal();
		result["finished_wiggle"] = finished_wiggle;
		return result;
	}
};

struct StorageWiggler : ReferenceCounted<StorageWiggler> {
	enum State : uint8_t { INVALID = 0, RUN = 1, PAUSE = 2 };
	AsyncVar<bool> nonEmpty;
	DDTeamCollection const* teamCollection;
	StorageWiggleMetrics metrics;
	// data structures
	typedef std::pair<StorageMetadataType, UID> MetadataUIDP;
	// min-heap
	boost::heap::skew_heap<MetadataUIDP, boost::heap::mutable_<true>, boost::heap::compare<std::greater<MetadataUIDP>>>
	    wiggle_pq;
	std::unordered_map<UID, decltype(wiggle_pq)::handle_type> pq_handles;

	State wiggleState = State::INVALID;
	double lastStateChangeTs = 0.0; // timestamp describes when did the state change

	explicit StorageWiggler(DDTeamCollection* collection) : nonEmpty(false), teamCollection(collection){};
	// add server to wiggling queue
	void addServer(const UID& serverId, const StorageMetadataType& metadata);
	// remove server from wiggling queue
	void removeServer(const UID& serverId);
	// update metadata and adjust priority_queue
	void updateMetadata(const UID& serverId, const StorageMetadataType& metadata);
	bool contains(const UID& serverId) const { return pq_handles.count(serverId) > 0; }
	bool empty() const { return wiggle_pq.empty(); }
	Optional<UID> getNextServerId();

	State getWiggleState() const { return wiggleState; }
	void setWiggleState(State s) {
		if (wiggleState != s) {
			wiggleState = s;
			lastStateChangeTs = g_network->now();
		}
	}
	static std::string getWiggleStateStr(State s) {
		switch (s) {
		case State::RUN:
			return "running";
		case State::PAUSE:
			return "paused";
		default:
			return "unknown";
		}
	}

	// -- statistic update

	// reset Statistic in database when perpetual wiggle is closed by user
	Future<Void> resetStats();
	// restore Statistic from database when the perpetual wiggle is opened
	Future<Void> restoreStats();
	// called when start wiggling a SS
	Future<Void> startWiggle();
	Future<Void> finishWiggle();
	bool shouldStartNewRound() const { return metrics.last_round_finish >= metrics.last_round_start; }
	bool shouldFinishRound() const {
		if (wiggle_pq.empty())
			return true;
		return (wiggle_pq.top().first.createdTime >= metrics.last_round_start);
	}
};

ACTOR Future<std::vector<std::pair<StorageServerInterface, ProcessClass>>> getServerListAndProcessClasses(
    Transaction* tr);
#include "flow/unactorcompiler.h"
#endif
