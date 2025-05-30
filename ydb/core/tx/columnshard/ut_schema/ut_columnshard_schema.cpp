#include <ydb/core/tx/columnshard/test_helper/columnshard_ut_common.h>
#include <ydb/core/base/tablet.h>
#include <ydb/core/wrappers/s3_wrapper.h>
#include <ydb/services/metadata/service.h>
#include <ydb/core/cms/console/configs_dispatcher.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/columnshard/test_helper/shard_reader.h>
#include <ydb/core/tx/columnshard/test_helper/test_combinator.h>
#include <ydb/core/tx/columnshard/hooks/abstract/abstract.h>
#include <ydb/core/tx/columnshard/hooks/testing/controller.h>
#include <ydb/core/tx/columnshard/blobs_reader/actor.h>
#include <ydb/core/tx/columnshard/test_helper/controllers.h>
#include <ydb/core/tx/columnshard/engines/changes/ttl.h>
#include <ydb/core/util/aws.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/table/table.h>

#include <ydb/library/actors/core/av_bootstrapped.h>

#include <util/system/hostname.h>
#include <library/cpp/deprecated/atomic/atomic.h>
#include <library/cpp/testing/hook/hook.h>


namespace NKikimr {

using namespace NTxUT;
using namespace NColumnShard;
using TDefaultTestsController = NKikimr::NYDBTest::NColumnShard::TController;

enum class EInitialEviction {
    None,
    Ttl,
    Tiering
};

namespace {

Y_TEST_HOOK_BEFORE_RUN(InitAwsAPI) {
    NKikimr::InitAwsAPI();
}

Y_TEST_HOOK_AFTER_RUN(ShutdownAwsAPI) {
    NKikimr::ShutdownAwsAPI();
}

static const std::vector<NArrow::NTest::TTestColumn> testYdbSchema = TTestSchema::YdbSchema();
static const std::vector<NArrow::NTest::TTestColumn> testYdbPk = TTestSchema::YdbPkSchema();

std::shared_ptr<arrow::RecordBatch> UpdateColumn(std::shared_ptr<arrow::RecordBatch> batch, TString columnName, i64 seconds) {
    std::string name(columnName.c_str(), columnName.size());

    auto schema = batch->schema();
    int pos = schema->GetFieldIndex(name);
    UNIT_ASSERT(pos >= 0);
    auto colType = batch->GetColumnByName(name)->type_id();

    std::shared_ptr<arrow::Array> array;
    if (colType == arrow::Type::TIMESTAMP) {
        auto scalar = arrow::TimestampScalar(seconds * 1000 * 1000, arrow::timestamp(arrow::TimeUnit::MICRO));
        UNIT_ASSERT_VALUES_EQUAL(scalar.value, seconds * 1000 * 1000);

        auto res = arrow::MakeArrayFromScalar(scalar, batch->num_rows());
        UNIT_ASSERT(res.ok());
        array = *res;
    } else if (colType == arrow::Type::UINT16) { // YQL Date
        TInstant date(TInstant::Seconds(seconds));
        auto res = arrow::MakeArrayFromScalar(arrow::UInt16Scalar(date.Days()), batch->num_rows());
        UNIT_ASSERT(res.ok());
        array = *res;
    } else if (colType == arrow::Type::UINT32) { // YQL Datetime or Uint32
        auto res = arrow::MakeArrayFromScalar(arrow::UInt32Scalar(seconds), batch->num_rows());
        UNIT_ASSERT(res.ok());
        array = *res;
    } else if (colType == arrow::Type::UINT64) {
        auto res = arrow::MakeArrayFromScalar(arrow::UInt64Scalar(seconds), batch->num_rows());
        UNIT_ASSERT(res.ok());
        array = *res;
    }

    UNIT_ASSERT(array);

    auto columns = batch->columns();
    columns[pos] = array;
    return arrow::RecordBatch::Make(schema, batch->num_rows(), columns);
}

bool TriggerMetadata(
    TTestBasicRuntime& runtime, TActorId& sender, NYDBTest::TControllers::TGuard<NOlap::TWaitCompactionController>& controller) {
    auto isDone = [initialCounter = controller->GetTieringMetadataActualizationCount().Val(), &controller]() {
        return controller->GetTieringMetadataActualizationCount().Val() != initialCounter;
    };

    auto event = std::make_unique<TEvPrivate::TEvPeriodicWakeup>();
    ForwardToTablet(runtime, TTestTxConfig::TxTablet0, sender, event.release());

    const TInstant deadline = TInstant::Now() + TDuration::Seconds(5);
    while (!isDone() && TInstant::Now() < deadline) {
        runtime.SimulateSleep(TDuration::Seconds(1));
    }
    return isDone();
}

bool CheckSame(const std::shared_ptr<arrow::RecordBatch>& batch, const ui32 expectedSize,
               const std::string& columnName, i64 seconds) {
    UNIT_ASSERT(batch);
    UNIT_ASSERT_VALUES_EQUAL(batch->num_rows(), expectedSize);
    auto tsCol = batch->GetColumnByName(columnName);
    UNIT_ASSERT(tsCol);

    std::shared_ptr<arrow::Scalar> expected;
    switch (tsCol->type_id()) {
        case arrow::Type::TIMESTAMP:
            expected = std::make_shared<arrow::TimestampScalar>(seconds * 1000 * 1000,
                                                                arrow::timestamp(arrow::TimeUnit::MICRO));
            break;
        case arrow::Type::UINT16:
            expected = std::make_shared<arrow::UInt16Scalar>(TInstant::Seconds(seconds).Days());
            break;
        case arrow::Type::UINT32:
            expected = std::make_shared<arrow::UInt32Scalar>(seconds);
            break;
        case arrow::Type::UINT64:
            expected = std::make_shared<arrow::UInt64Scalar>(seconds);
            break;
        default:
            break;
    }

    UNIT_ASSERT(expected);

    for (int i = 0; i < tsCol->length(); ++i) {
        auto value = *tsCol->GetScalar(i);
        if (!value->Equals(*expected)) {
            Cerr << "Unexpected: '" << value->ToString() << "', expected " << expected->ToString() << "\n";
            return false;
        }
    }
    return true;
}

std::vector<TString> MakeData(const std::vector<ui64>& ts, ui32 portionSize, ui32 overlapSize, const TString& ttlColumnName,
                              const std::vector<NArrow::NTest::TTestColumn>& ydbSchema = testYdbSchema) {
    UNIT_ASSERT(ts.size() > 0);

    ui32 numRows = portionSize + (ts.size() - 1) * (portionSize - overlapSize);
    TString testData = MakeTestBlob({0, numRows}, ydbSchema);
    auto schema = NArrow::MakeArrowSchema(ydbSchema);
    auto testBatch = NArrow::DeserializeBatch(testData, schema);

    std::vector<TString> data;
    data.reserve(ts.size());
    for (size_t i = 0; i < ts.size(); ++i) {
        auto batch = testBatch->Slice((portionSize - overlapSize) * i, portionSize);
        batch = UpdateColumn(batch, ttlColumnName, ts[i]);
        data.emplace_back(NArrow::SerializeBatchNoCompression(batch));
        UNIT_ASSERT(data.back().size() > NColumnShard::TLimits::MIN_BYTES_TO_INSERT);
    }

    return data;
}

enum class EExpectedResult {
    OK_FINISHED,
    OK,
    ERROR
};

static constexpr ui32 PORTION_ROWS = 80 * 1000;

// ts[0] = 1600000000; // date -u --date='@1600000000' Sun Sep 13 12:26:40 UTC 2020
// ts[1] = 1620000000; // date -u --date='@1620000000' Mon May  3 00:00:00 UTC 2021
void TestTtl(bool reboots, bool internal, bool useFirstPkColumnForTtl, NScheme::TTypeId ttlColumnTypeId)
{
    auto csControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<NOlap::TWaitCompactionController>();
    csControllerGuard->DisableBackground(NKikimr::NYDBTest::ICSController::EBackground::Compaction);
    csControllerGuard->SetOverrideTasksActualizationLag(TDuration::Zero());
    std::vector<ui64> ts = { 1600000000, 1620000000 };

    auto ydbSchema = TTestSchema::YdbSchema();
    auto ydbPk = TTestSchema::YdbPkSchema();
    const ui64 ttlColumnNameIdx = useFirstPkColumnForTtl ? 0 : 8;
    const auto ttlColumnName = ydbSchema[ttlColumnNameIdx].GetName();
    UNIT_ASSERT(ttlColumnName == (useFirstPkColumnForTtl ? "timestamp" : "saved_at")); //to detect default schema changes
    ydbSchema[ttlColumnNameIdx].SetType(ttlColumnTypeId);
    if (ttlColumnNameIdx < ydbPk.size()) {
        ydbPk[ttlColumnNameIdx].SetType(ttlColumnTypeId);
    }
    const auto ttlIncSeconds = ttlColumnTypeId == NTypeIds::Date ? TDuration::Days(1).Seconds() : 1;
    TTestSchema::TTableSpecials specs;

    TTestBasicRuntime runtime;
    TTester::Setup(runtime);
    runtime.SetLogPriority(NKikimrServices::TX_COLUMNSHARD, NActors::NLog::PRI_TRACE);

    TActorId sender = runtime.AllocateEdgeActor();
    CreateTestBootstrapper(runtime,
                           CreateTestTabletInfo(TTestTxConfig::TxTablet0, TTabletTypes::ColumnShard),
                           &CreateColumnShard);

    TDispatchOptions options;
    options.FinalEvents.push_back(TDispatchOptions::TFinalEventCondition(TEvTablet::EvBoot));
    runtime.DispatchEvents(options);

    //

    ui64 writeId = 0;
    ui64 tableId = 1;
    ui64 txId = 100;

    UNIT_ASSERT(ts.size() == 2);

    ui32 ttlSec = TAppData::TimeProvider->Now().Seconds(); // disable internal tll
    if (internal) {
        ttlSec -= (ts[0] + ts[1]) / 2; // enable internal ttl between ts1 and ts2
    } else {
        ttlSec -= ts[0] + ttlIncSeconds;
    }
    TTestSchema::TTableSpecials spec;
    spec.TtlColumn = ttlColumnName;
    spec.EvictAfter = TDuration::Seconds(ttlSec);
    auto planStep = SetupSchema(runtime, sender, TTestSchema::CreateInitShardTxBody(tableId, ydbSchema, ydbPk, spec, "/Root/olapStore"), ++txId);
    if (spec.HasTiers()) {
        csControllerGuard->OverrideTierConfigs(runtime, sender, TTestSchema::BuildSnapshot(spec));
    }
    //

    auto blobs = MakeData(ts, PORTION_ROWS, PORTION_ROWS / 2, spec.TtlColumn, ydbSchema);
    UNIT_ASSERT_EQUAL(blobs.size(), 2);
    auto lastTtlFinishedCount = csControllerGuard->GetTTLFinishedCounter().Val();
    for (auto& data : blobs) {
        std::vector<ui64> writeIds;
        UNIT_ASSERT(WriteData(runtime, sender, ++writeId, tableId, data, ydbSchema, true, &writeIds));
        planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
        PlanCommit(runtime, sender, planStep, txId);
    }

    // TODO: write into path 2 (no ttl)

    if (reboots) {
        RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
    }

    ForwardToTablet(runtime, TTestTxConfig::TxTablet0, sender, new TEvPrivate::TEvPeriodicWakeup(true));
    while (csControllerGuard->GetTTLFinishedCounter().Val() == lastTtlFinishedCount) {
        runtime.SimulateSleep(TDuration::Seconds(1)); // wait all finished before (ttl especially)
    }

    TAutoPtr<IEventHandle> handle;

    if (reboots) {
        RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
    }

    {
        TShardReader reader(runtime, TTestTxConfig::TxTablet0, tableId, NOlap::TSnapshot(planStep, Max<ui64>()));
        reader.SetReplyColumnIds(TTestSchema::GetColumnIds(TTestSchema::YdbSchema(), { spec.TtlColumn }));
        auto rb = reader.ReadAll();
        UNIT_ASSERT(reader.IsCorrectlyFinished());
        UNIT_ASSERT(CheckSame(rb, PORTION_ROWS, spec.TtlColumn, ts[1]));
    }

    // Alter TTL
    ttlSec = TAppData::TimeProvider->Now().Seconds() - (ts[1] + 1);
    if (spec.HasTiers()) {
        spec.Tiers[0].EvictAfter = TDuration::Seconds(ttlSec);
    } else {
        spec.EvictAfter = TDuration::Seconds(ttlSec);
    }
    planStep = SetupSchema(runtime, sender, TTestSchema::AlterTableTxBody(tableId, 2, ydbSchema, ydbPk, spec), ++txId);
    if (spec.HasTiers()) {
        csControllerGuard->OverrideTierConfigs(runtime, sender, TTestSchema::BuildSnapshot(spec));
    }
    lastTtlFinishedCount = csControllerGuard->GetTTLFinishedCounter().Val();
    ForwardToTablet(runtime, TTestTxConfig::TxTablet0, sender, new TEvPrivate::TEvPeriodicWakeup(true));
    while (csControllerGuard->GetTTLFinishedCounter().Val() == lastTtlFinishedCount) {
        runtime.SimulateSleep(TDuration::Seconds(1)); // wait all finished before (ttl especially)
    }

    {
        TShardReader reader(runtime, TTestTxConfig::TxTablet0, tableId, NOlap::TSnapshot(planStep, Max<ui64>()));
        auto columnIds = TTestSchema::GetColumnIds(TTestSchema::YdbSchema(), { spec.TtlColumn });
        columnIds.emplace_back((ui32)NOlap::IIndexInfo::ESpecialColumn::PLAN_STEP);
        reader.SetReplyColumnIds(columnIds);
        auto rb = reader.ReadAll();
        UNIT_ASSERT(reader.IsCorrectlyFinished());
        UNIT_ASSERT(!rb || !rb->num_rows());
    }

    // Disable TTL
    lastTtlFinishedCount = csControllerGuard->GetTTLFinishedCounter().Val();
    planStep = SetupSchema(runtime, sender, TTestSchema::AlterTableTxBody(tableId, 3, ydbSchema, ydbPk, TTestSchema::TTableSpecials()), ++txId);
    if (spec.HasTiers()) {
        csControllerGuard->OverrideTierConfigs(runtime, sender, TTestSchema::BuildSnapshot(TTestSchema::TTableSpecials()));
    }
    PlanSchemaTx(runtime, sender, NOlap::TSnapshot(planStep, txId));


    std::vector<ui64> writeIds;
    UNIT_ASSERT(WriteData(runtime, sender, ++writeId, tableId, blobs[0], ydbSchema, true, &writeIds));
    planStep  = ProposeCommit(runtime, sender, ++txId, writeIds);
    PlanCommit(runtime, sender, planStep, txId);

    ForwardToTablet(runtime, TTestTxConfig::TxTablet0, sender, new TEvPrivate::TEvPeriodicWakeup(true));

    {
        TShardReader reader(runtime, TTestTxConfig::TxTablet0, tableId, NOlap::TSnapshot(planStep, Max<ui64>()));
        reader.SetReplyColumnIds(TTestSchema::GetColumnIds(TTestSchema::YdbSchema(), { spec.TtlColumn }));
        auto rb = reader.ReadAll();
        UNIT_ASSERT(reader.IsCorrectlyFinished());
        UNIT_ASSERT(CheckSame(rb, PORTION_ROWS, spec.TtlColumn, ts[0]));
    }

    if (spec.NeedTestStatistics(ydbPk)) {
        AFL_VERIFY(csControllerGuard->GetStatisticsUsageCount().Val());
        AFL_VERIFY(!csControllerGuard->GetMaxValueUsageCount().Val());
    } else {
        AFL_VERIFY(!csControllerGuard->GetStatisticsUsageCount().Val());
        AFL_VERIFY(csControllerGuard->GetMaxValueUsageCount().Val());
    }
}

class TCountersContainer {
private:
    struct TCounters {
        ui32 Attempt = 0;
        ui32 Request = 0;
        ui32 Response = 0;
        ui32 Success = 0;

        void Clear() {
            Attempt = 0;
            Request = 0;
            Response = 0;
            Success = 0;
        }

        TString ToString() const {
            return TStringBuilder() << Attempt << "/" << Request << "/" << Response << "/" << Success;
        }
    };

    ui32 WaitNo = 0;

public:
    TCounters ExportCounters;
    TCounters ForgetCounters;
    ui32 CaptureReadEvents = 0;
    std::vector<TAutoPtr<IEventHandle>> CapturedReads;
    ui32 CaptureEvictResponse = 0;
    ui32 CaptureForgetResponse = 0;
    std::vector<TAutoPtr<IEventHandle>> CapturedResponses;
    bool BlockForgets = false;

    void WaitEvents(TTestBasicRuntime& runtime, const TDuration& timeout, ui32 waitExports, ui32 waitForgets,
                    const TString& promo = "START_WAITING") {
        const TInstant startInstant = TAppData::TimeProvider->Now();
        const TInstant deadline = startInstant + timeout;
        Cerr << promo << "(" << WaitNo << "): "
            << "E" << ExportCounters.ToString() << " F" << ForgetCounters.ToString() << Endl;
        while (TAppData::TimeProvider->Now() < deadline) {
            Cerr << "IN_WAITING(" << WaitNo << "): "
                << "E" << ExportCounters.ToString() << " F" << ForgetCounters.ToString() << Endl;
            runtime.SimulateSleep(TDuration::Seconds(1));

            if (!waitExports && ExportCounters.Success
                || !waitForgets && ForgetCounters.Success
                || !waitForgets && ExportCounters.Success >= waitExports
                || !waitExports && ForgetCounters.Success >= waitForgets
                || waitExports && waitForgets
                    && ExportCounters.Success >= waitExports && ForgetCounters.Success >= waitForgets) {
                break;
            }
        }
        Cerr << "FINISH_WAITING(" << WaitNo << "): "
            << "E" << ExportCounters.ToString() << " F" << ForgetCounters.ToString() << Endl;
        UNIT_ASSERT_VALUES_EQUAL(ExportCounters.Success, waitExports);
        UNIT_ASSERT_VALUES_EQUAL(ForgetCounters.Success, waitForgets);
        ExportCounters.Clear();
        ForgetCounters.Clear();
        ++WaitNo;
    }

    void WaitMoreEvents(TTestBasicRuntime& runtime, const TDuration& timeout, ui32 waitExports, ui32 waitForgets) {
        --WaitNo;
        WaitEvents(runtime, timeout, waitExports, waitForgets, "CONTINUE_WAITING");
    }

    void WaitReadsCaptured(TTestBasicRuntime& runtime) const {
        const TInstant startInstant = TAppData::TimeProvider->Now();
        const TInstant deadline = startInstant + TDuration::Seconds(10);
        while (CaptureReadEvents && TAppData::TimeProvider->Now() < deadline) {
            runtime.SimulateSleep(TDuration::Seconds(1));
        }
//        UNIT_ASSERT_VALUES_EQUAL(CaptureReadEvents, 0);
    }

    void ResendCapturedReads(TTestBasicRuntime& runtime) {
        for (auto& cev : CapturedReads) {
            auto* msg = TryGetPrivateEvent<NBlobCache::TEvBlobCache::TEvReadBlobRangeBatch>(cev);
            UNIT_ASSERT(msg);
            Cerr << "RESEND " << JoinSeq(",", msg->BlobRanges) << " "
                    << msg->ReadOptions.ToString() << Endl;
            runtime.Send(cev.Release());
        }
        CapturedReads.clear();
    }

    void ResendCapturedResponses(TTestBasicRuntime& runtime) {
        for (auto& cev : CapturedResponses) {
            Cerr << "RESEND S3_RESPONSE" << Endl;
            runtime.Send(cev.Release());
        }
        CapturedResponses.clear();
    }

    void BlockForgetsTillReboot() {
        BlockForgets = true;
    }
};

class TEventsCounter {
private:
    TCountersContainer* Counters = nullptr;
    TTestBasicRuntime& Runtime;

public:
    TEventsCounter(TCountersContainer& counters, TTestBasicRuntime& runtime)
        : Counters(&counters)
        , Runtime(runtime)
    {
        Y_UNUSED(Runtime);
    }

    bool operator()(TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& ev) {
        TStringBuilder ss;
        if (ev->GetTypeRewrite() == TEvTablet::EvBoot) {
            Counters->BlockForgets = false;
            return false;
        } else if (auto* msg = TryGetPrivateEvent<NWrappers::NExternalStorage::TEvPutObjectRequest>(ev)) {
            ss << "S3_REQ(put " << ++Counters->ExportCounters.Request << "):";
        } else if (auto* msg = TryGetPrivateEvent<NWrappers::NExternalStorage::TEvPutObjectResponse>(ev)) {
            if (Counters->CaptureEvictResponse) {
                Cerr << "CAPTURE S3_RESPONSE(put)" << Endl;
                --Counters->CaptureEvictResponse;
                Counters->CapturedResponses.push_back(ev.Release());
                return true;
            }

            ss << "S3_RESPONSE(put " << ++Counters->ExportCounters.Response << "):";
        } else if (auto* msg = TryGetPrivateEvent<NWrappers::NExternalStorage::TEvDeleteObjectRequest>(ev)) {
            ss << "S3_REQ(delete " << ++Counters->ForgetCounters.Request << "):";
        } else if (auto* msg = TryGetPrivateEvent<NWrappers::NExternalStorage::TEvDeleteObjectResponse>(ev)) {
            if (Counters->CaptureForgetResponse) {
                Cerr << "CAPTURE S3_RESPONSE(delete)" << Endl;
                --Counters->CaptureForgetResponse;
                Counters->CapturedResponses.push_back(ev.Release());
                return true;
            }

            ss << "S3_RESPONSE(delete " << ++Counters->ForgetCounters.Response << "):";
        } else if (auto* msg = TryGetPrivateEvent<NBlobCache::TEvBlobCache::TEvReadBlobRangeBatch>(ev)) {
            if (Counters->CaptureReadEvents) {
                Cerr << "CAPTURE " << JoinSeq(",", msg->BlobRanges) << " "
                    << msg->ReadOptions.ToString() << Endl;
                --Counters->CaptureReadEvents;
                Counters->CapturedReads.push_back(ev.Release());
                return true;
            } else {
                return false;
            }
        } else if (auto* msg = TryGetPrivateEvent<NKqp::TEvKqpCompute::TEvScanData>(ev)) {
            ss << "Got TEvKqpCompute::TEvScanData" << Endl;
        } else {
            return false;
        }
        ss << " " << ev->Sender << "->" << ev->Recipient;
        Cerr << ss << Endl;
        return false;
    }
};

std::vector<std::pair<ui32, ui64>> TestTiers(bool reboots, const std::vector<TString>& blobs,
                                             const std::vector<TTestSchema::TTableSpecials>& specsExt,
                                             std::optional<ui32> eventLoss = {}, const bool buildTTL = true)
{
    auto specs = specsExt;
    if (buildTTL) {
        for (auto&& i : specs) {
            if (!i.HasTtl() && i.HasTiers()) {
                std::optional<TDuration> d;
                for (auto&& i : i.Tiers) {
                    if (!d || *d < i.EvictAfter) {
                        d = i.EvictAfter;
                    }
                }
                Y_ABORT_UNLESS(d);
                i.SetTtl(*d);
            }
        }
    }
    for (auto&& s : specs) {
        Cerr << s.DebugString() << Endl;
    }

    auto csControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<NOlap::TWaitCompactionController>();
    csControllerGuard->DisableBackground(NYDBTest::ICSController::EBackground::TTL);
    csControllerGuard->SetOverrideTasksActualizationLag(TDuration::Zero());
    TTestBasicRuntime runtime;
    TTester::Setup(runtime);

    runtime.SetLogPriority(NKikimrServices::TX_COLUMNSHARD, NActors::NLog::PRI_DEBUG);
    runtime.SetLogPriority(NKikimrServices::TX_COLUMNSHARD_SCAN, NActors::NLog::PRI_DEBUG);

    TActorId sender = runtime.AllocateEdgeActor();
    CreateTestBootstrapper(runtime,
                           CreateTestTabletInfo(TTestTxConfig::TxTablet0, TTabletTypes::ColumnShard),
                           &CreateColumnShard);

    TDispatchOptions options;
    options.FinalEvents.push_back(TDispatchOptions::TFinalEventCondition(TEvTablet::EvBoot));
    runtime.DispatchEvents(options);

    // Disable blob cache. It hides evict-delete, evict-read races.
    {
        TAtomic unused;
        runtime.GetAppData().Icb->SetValue("BlobCache.MaxCacheDataSize", 0, unused);
    }

    ui64 writeId = 0;
    ui64 tableId = 1;
    ui64 txId = 100;
//    const TDuration exportTimeout = TDuration::Seconds(40);

    UNIT_ASSERT(specs.size() > 0);
    auto planStep = SetupSchema(runtime, sender,
            TTestSchema::CreateInitShardTxBody(tableId, testYdbSchema, testYdbPk, specs[0], "/Root/olapStore"), ++txId);
    if (specs[0].Tiers.size()) {
        csControllerGuard->OverrideTierConfigs(runtime, sender, TTestSchema::BuildSnapshot(specs[0]));
    }

    for (auto& data : blobs) {
        std::vector<ui64> writeIds;
        UNIT_ASSERT(WriteData(runtime, sender, ++writeId, tableId, data, testYdbSchema, true, &writeIds));
        planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
        PlanCommit(runtime, sender, planStep, txId);
    }

    if (reboots) {
        RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
    }
    csControllerGuard->EnableBackground(NYDBTest::ICSController::EBackground::TTL);

    runtime.SetLogPriority(NKikimrServices::TX_COLUMNSHARD, NActors::NLog::PRI_DEBUG);

    TAutoPtr<IEventHandle> handle;

    std::vector<std::pair<ui32, ui64>> specRowsBytes;
    specRowsBytes.reserve(specs.size());

    TCountersContainer counter;
    runtime.SetEventFilter(TEventsCounter(counter, runtime));
    for (ui32 i = 0; i < specs.size(); ++i) {
        NActors::TLogContextGuard logGuard = NActors::TLogContextBuilder::Build(NKikimrServices::TX_COLUMNSHARD)("TEST_STEP", i);
        bool hasColdEviction = false;
        bool misconfig = false;
        auto expectedReadResult = EExpectedResult::OK;
        ui32 tIdx = 0;
        std::optional<ui32> tIdxCorrect;
        TString originalEndpoint;
        for (auto&& spec : specs[i].Tiers) {
            hasColdEviction = true;
            if (spec.S3.GetEndpoint() != "fake.fake") {
                misconfig = true;
                // misconfig in export => OK, misconfig after export => ERROR
                if (i > 1) {
                    expectedReadResult = EExpectedResult::ERROR;
                }
                originalEndpoint = spec.S3.GetEndpoint();
                spec.S3.SetEndpoint("fake.fake");
                tIdxCorrect = tIdx++;
            }
            break;
        }
        if (i) {
            const ui32 version = 2 * i + 1;
            planStep = SetupSchema(runtime, sender, TTestSchema::AlterTableTxBody(tableId, version, testYdbSchema, testYdbPk, specs[i]), ++txId);
        }
        if (specs[i].HasTiers() || reboots) {
            csControllerGuard->OverrideTierConfigs(runtime, sender, TTestSchema::BuildSnapshot(specs[i]));
        }
        UNIT_ASSERT(TriggerMetadata(runtime, sender, csControllerGuard));

        if (eventLoss) {
            if (*eventLoss == i) {
                counter.CaptureEvictResponse = 1;
            } else {
                // Check there would be no troubles with delayed responses
                counter.ResendCapturedResponses(runtime);
            }
        }

        // Read crossed with eviction (start)
        {
            std::unique_ptr<TShardReader> reader;
            if (!misconfig) {
                reader = std::make_unique<TShardReader>(runtime, TTestTxConfig::TxTablet0, tableId, NOlap::TSnapshot(planStep - 1, Max<ui64>()));
                reader->SetReplyColumnIds(TTestSchema::GetColumnIds(TTestSchema::YdbSchema(), { specs[i].TtlColumn }));
                counter.CaptureReadEvents = specs[i].WaitEmptyAfter ? 0 : 1; // TODO: we need affected by tiering blob here
                counter.WaitReadsCaptured(runtime);
                reader->InitializeScanner();
                reader->Ack();
            }
            // Eviction
            ForwardToTablet(runtime, TTestTxConfig::TxTablet0, sender, new TEvPrivate::TEvPeriodicWakeup(true));

            Cerr << "-- " << (hasColdEviction ? "COLD" : "HOT")
                << " TIERING(" << i << ") num tiers: " << specs[i].Tiers.size() << Endl;

            // Read crossed with eviction (finish)
            if (!misconfig) {
                counter.ResendCapturedReads(runtime);
                reader->ContinueReadAll();
                UNIT_ASSERT(reader->IsCorrectlyFinished());
            }
        }
        while (csControllerGuard->GetTTLFinishedCounter().Val() != csControllerGuard->GetTTLStartedCounter().Val()) {
            runtime.SimulateSleep(TDuration::Seconds(1)); // wait all finished before (ttl especially)
        }

        if (tIdxCorrect) {
            specs[i].Tiers[*tIdxCorrect].S3.SetEndpoint(originalEndpoint);
            csControllerGuard->OverrideTierConfigs(runtime, sender, TTestSchema::BuildSnapshot(specs[i]));
        }


        if (reboots) {
            Cerr << "INTERMEDIATE REBOOT(" << i << ")" << Endl;
            RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
        }

        // Read data after eviction
        TString columnToRead = specs[i].TtlColumn;

        TShardReader reader(runtime, TTestTxConfig::TxTablet0, tableId, NOlap::TSnapshot(planStep, Max<ui64>()));
        reader.SetReplyColumnIds(TTestSchema::GetColumnIds(TTestSchema::YdbSchema(), { columnToRead }));
        auto rb = reader.ReadAll();
        if (expectedReadResult == EExpectedResult::ERROR) {
            UNIT_ASSERT(reader.IsError());
            specRowsBytes.emplace_back(0, 0);
        } else {
            UNIT_ASSERT(reader.IsCorrectlyFinished());
            specRowsBytes.emplace_back(reader.GetRecordsCount(), reader.GetReadBytes());
        }

        UNIT_ASSERT(reader.GetIterationsCount() < 100);

        if (reboots) {
            Cerr << "REBOOT(" << i << ")" << Endl;
            RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
        } else if (misconfig) {
            while (NOlap::NBlobOperations::NRead::TActor::WaitingBlobsCount.Val()) {
                runtime.SimulateSleep(TDuration::Seconds(1));
                AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("waiting", NOlap::NBlobOperations::NRead::TActor::WaitingBlobsCount.Val());
            }
        }
    }

//    if (specs[0].TtlColumn != testYdbPk.front().GetName()) {
//        AFL_VERIFY(csControllerGuard->GetStatisticsUsageCount().Val());
//        AFL_VERIFY(!csControllerGuard->GetMaxValueUsageCount().Val());
//    } else {
//        AFL_VERIFY(!csControllerGuard->GetStatisticsUsageCount().Val());
//        AFL_VERIFY(csControllerGuard->GetMaxValueUsageCount().Val());
//    }

    return specRowsBytes;
}

class TEvictionChanges {
public:
    static std::vector<TTestSchema::TTableSpecials> OneTierAlters(const TTestSchema::TTableSpecials& spec,
                                                                const std::vector<ui64>& ts) {
        TInstant now = TAppData::TimeProvider->Now();
        TDuration allowBoth = TDuration::Seconds(now.Seconds() - ts[0] + 600);
        TDuration allowOne = TDuration::Seconds(now.Seconds() - ts[1] + 600);
        TDuration allowNone = TDuration::Seconds(now.Seconds() - ts[1] - 600);

        std::vector<TTestSchema::TTableSpecials> alters = { TTestSchema::TTableSpecials() };
        AddTierAlters(spec, {allowBoth, allowOne, allowNone}, alters);
        return alters;
    }

    static void AddTierAlters(const TTestSchema::TTableSpecials& spec, const std::vector<TDuration>&& borders,
                        std::vector<TTestSchema::TTableSpecials>& alters) {
        UNIT_ASSERT_EQUAL(borders.size(), 3);
        UNIT_ASSERT(spec.Tiers.size());

        if (spec.Tiers.size() == 1) {
            alters.push_back(MakeAlter(spec, {borders[0]})); // <tier0 border>, data[0], data[1]
            alters.push_back(MakeAlter(spec, {borders[1]})); // data[0], <tier0 border>, data[1]
            alters.push_back(MakeAlter(spec, {borders[2]})); // data[0], data[1], <tier0 border>
        } else if (spec.Tiers.size() == 2) {
            alters.push_back(MakeAlter(spec, {borders[0], borders[0]})); // <tier1 border>, <tier0 border>, data[0], data[1]
            alters.push_back(MakeAlter(spec, {borders[1], borders[0]})); // <tier1 border>, data[0], <tier0 border>, data[1]
            alters.push_back(MakeAlter(spec, {borders[2], borders[1]})); // data[0], <tier1 border>, data[1], <tier0 border>
            alters.push_back(MakeAlter(spec, {borders[2], borders[2]})); // data[0], data[1], <tier1 border>, <tier0 border>
        }
    }

    static void AddTtlAlters(const TTestSchema::TTableSpecials& spec, const std::vector<TDuration>&& borders,
                      std::vector<TTestSchema::TTableSpecials>& alters) {
        UNIT_ASSERT_EQUAL(borders.size(), 3);
        UNIT_ASSERT(spec.Tiers.size());

        TTestSchema::TTableSpecials specTtl(spec);
        if (spec.Tiers.size() == 1) {
            specTtl = MakeAlter(spec, {borders[0]}); // <tier0 border>, data[0], data[1]
        } else if (spec.Tiers.size() == 2) {
            specTtl = MakeAlter(spec, {borders[0], borders[0]}); // <tier1 border>, <tier0 border>, data[0], data[1]
        }

        alters.reserve(alters.size() + borders.size());
        alters.push_back(specTtl.SetTtl(borders[0])); // <ttl border>, data[0], data[1]
        alters.push_back(specTtl.SetTtl(borders[1])); // data[0], <ttl border>, data[1]
        alters.push_back(specTtl.SetTtl(borders[2])); // data[0], data[1], <ttl border>
    }

    static void Assert(const TTestSchema::TTableSpecials& spec,
                       const std::vector<std::pair<ui32, ui64>>& rowsBytes,
                       size_t initialEviction) {
        UNIT_ASSERT_VALUES_EQUAL(rowsBytes[initialEviction].first, 2 * PORTION_ROWS);
        UNIT_ASSERT(rowsBytes[initialEviction].second);
        if (spec.Tiers.size() > 1) {
            UNIT_ASSERT_VALUES_EQUAL(rowsBytes[initialEviction].first, rowsBytes[initialEviction + 1].first);
        }

        UNIT_ASSERT_VALUES_EQUAL(rowsBytes[rowsBytes.size() - 2].first, PORTION_ROWS);
        UNIT_ASSERT(rowsBytes[rowsBytes.size() - 2].second);

        UNIT_ASSERT_VALUES_EQUAL(rowsBytes.back().first, 0);
        UNIT_ASSERT_VALUES_EQUAL(rowsBytes.back().second, 0);
    }

private:
    static TTestSchema::TTableSpecials MakeAlter(const TTestSchema::TTableSpecials& spec,
                                          const std::vector<TDuration>& tierBorders) {
        UNIT_ASSERT_EQUAL(spec.Tiers.size(), tierBorders.size());

        TTestSchema::TTableSpecials alter(spec); // same TTL, Codec, etc.
        for (size_t i = 0; i < tierBorders.size(); ++i) {
            alter.Tiers[i].EvictAfter = tierBorders[i];
        }
        return alter;
    }
};

TTestSchema::TTableSpecials InitialSpec(const EInitialEviction init, TDuration initTs) {
    TTestSchema::TTableSpecials spec;
    spec.TtlColumn = "timestamp";
    if (init == EInitialEviction::Ttl) {
        spec.EvictAfter = initTs;
    }
    return spec;
}

std::vector<std::pair<ui32, ui64>> TestTiersAndTtl(const TTestSchema::TTableSpecials& spec, bool reboots,
                                                   EInitialEviction init, bool testTtl = false) {
    const std::vector<ui64> ts = { 1600000000, 1620000000 };

    ui32 overlapSize = 0; // TODO: 40 * 1000 (it should lead to fewer row count in result)
    std::vector<TString> blobs = MakeData(ts, PORTION_ROWS, overlapSize, spec.TtlColumn);
    if (init != EInitialEviction::Tiering) {
        std::vector<TString> preload = MakeData({ 1500000000, 1620000000 }, PORTION_ROWS, overlapSize, spec.TtlColumn);
        blobs.emplace_back(std::move(preload[0]));
        blobs.emplace_back(std::move(preload[1]));
    }

    TInstant now = TAppData::TimeProvider->Now();
    TDuration allowBoth = TDuration::Seconds(now.Seconds() - ts[0] + 600);
    TDuration allowOne = TDuration::Seconds(now.Seconds() - ts[1] + 600);
    TDuration allowNone = TDuration::Seconds(now.Seconds() - ts[1] - 600);

    std::vector<TTestSchema::TTableSpecials> alters = { InitialSpec(init, allowBoth) };
    size_t initialEviction = alters.size();

    TEvictionChanges changes;
    if (testTtl) {
        changes.AddTtlAlters(spec, {allowBoth, allowOne, allowNone}, alters);
        alters.back().WaitEmptyAfter = true;
    } else {
        changes.AddTierAlters(spec, {allowBoth, allowOne, allowNone}, alters);
    }

    auto rowsBytes = TestTiers(reboots, blobs, alters);
    for (auto&& i : rowsBytes) {
        Cerr << i.first << "/" << i.second << Endl;
    }

    UNIT_ASSERT_EQUAL(rowsBytes.size(), alters.size());

    if (!testTtl) { // TODO
        changes.Assert(spec, rowsBytes, initialEviction);
    }
    return rowsBytes;
}

std::vector<std::pair<ui32, ui64>> TestOneTierExport(const TTestSchema::TTableSpecials& spec,
                                                    const std::vector<TTestSchema::TTableSpecials>& alters,
                                                    const std::vector<ui64>& ts, bool reboots, std::optional<ui32> loss, const bool buildTTL = true) {
    ui32 overlapSize = 0;
    std::vector<TString> blobs = MakeData(ts, PORTION_ROWS, overlapSize, spec.TtlColumn);

    auto rowsBytes = TestTiers(reboots, blobs, alters, loss, buildTTL);
    for (auto&& i : rowsBytes) {
        Cerr << i.first << "/" << i.second << Endl;
    }

    UNIT_ASSERT_EQUAL(rowsBytes.size(), alters.size());
    return rowsBytes;
}

void TestTwoHotTiers(bool reboot, bool changeTtl, const EInitialEviction initial = EInitialEviction::None,
                    bool revCompaction = false) {
    TTestSchema::TTableSpecials spec;
    spec.SetTtlColumn("timestamp");
    spec.Tiers.emplace_back(TTestSchema::TStorageTier("tier0").SetTtlColumn("timestamp"));
    spec.Tiers.emplace_back(TTestSchema::TStorageTier("tier1").SetTtlColumn("timestamp"));
    spec.Tiers[(revCompaction ? 0 : 1)].SetCodec("zstd");

    auto rowsBytes = TestTiersAndTtl(spec, reboot, initial, changeTtl);
    if (changeTtl) {
        UNIT_ASSERT_VALUES_EQUAL(rowsBytes.size(), 4);
        UNIT_ASSERT_VALUES_EQUAL(rowsBytes[0].first, 3 * PORTION_ROWS);
        UNIT_ASSERT_VALUES_EQUAL(rowsBytes[1].first, 2 * PORTION_ROWS);
        UNIT_ASSERT_VALUES_EQUAL(rowsBytes[2].first, PORTION_ROWS);
        UNIT_ASSERT_VALUES_EQUAL(rowsBytes[3].first, 0);
    } else {
        UNIT_ASSERT_VALUES_EQUAL(rowsBytes.size(), 5);
        if (initial == EInitialEviction::Ttl) {
            UNIT_ASSERT_VALUES_EQUAL(rowsBytes[0].first, 2 * PORTION_ROWS);
        } else {
            UNIT_ASSERT_VALUES_EQUAL(rowsBytes[0].first, 3 * PORTION_ROWS);
        }
        UNIT_ASSERT_VALUES_EQUAL(rowsBytes[1].first, 2 * PORTION_ROWS);
        UNIT_ASSERT_VALUES_EQUAL(rowsBytes[2].first, 2 * PORTION_ROWS);
        UNIT_ASSERT_VALUES_EQUAL(rowsBytes[3].first, PORTION_ROWS);
        UNIT_ASSERT_VALUES_EQUAL(rowsBytes[4].first, 0);

        // compression works
        if (revCompaction) {
//            UNIT_ASSERT(rowsBytes[1].second < rowsBytes[2].second);
        } else {
//            UNIT_ASSERT(rowsBytes[1].second > rowsBytes[2].second);
        }
    }
}

void TestHotAndColdTiers(bool reboot, const EInitialEviction initial) {
    TTestSchema::TTableSpecials spec;
    spec.SetTtlColumn("timestamp");
    spec.Tiers.emplace_back(TTestSchema::TStorageTier("tier0").SetTtlColumn("timestamp"));
    spec.Tiers.emplace_back(TTestSchema::TStorageTier("tier1").SetTtlColumn("timestamp"));
    spec.Tiers.back().S3 = TTestSchema::TStorageTier::FakeS3();
    TestTiersAndTtl(spec, reboot, initial);
}

struct TExportTestOpts {
    std::optional<ui32> Misconfig;
    std::optional<ui32> Loss;
    std::optional<ui32> NoTier;
};

void TestExport(bool reboot, TExportTestOpts&& opts = TExportTestOpts{}) {
    TTestSchema::TTableSpecials spec;
    spec.SetTtlColumn("timestamp");
    spec.Tiers.emplace_back(TTestSchema::TStorageTier("cold").SetTtlColumn("timestamp"));
    spec.Tiers.back().S3 = TTestSchema::TStorageTier::FakeS3();

    const std::vector<ui64> ts = { 1600000000, 1620000000 };
    TEvictionChanges changes;
    std::vector<TTestSchema::TTableSpecials> alters = changes.OneTierAlters(spec, ts);
    UNIT_ASSERT_VALUES_EQUAL(alters.size(), 4);

    if (opts.Misconfig) {
        ui32 alterNo = *opts.Misconfig;
        // Add error in config => eviction + not finished export
        UNIT_ASSERT_VALUES_EQUAL(alters[alterNo].Tiers.size(), 1);
        alters[alterNo].Tiers[0].S3.SetEndpoint("nowhere"); // clear special "fake" endpoint
    }
    if (opts.NoTier) {
        ui32 alterNo = *opts.NoTier;
        // Add error in config => eviction + not finished export
        UNIT_ASSERT_VALUES_EQUAL(alters[alterNo].Tiers.size(), 1);
        alters[alterNo].Tiers.clear();
    }

    auto rowsBytes = TestOneTierExport(spec, alters, ts, reboot, opts.Loss, !opts.Misconfig);
    if (!opts.Misconfig) {
        changes.Assert(spec, rowsBytes, 1);
    }
}

void TestDrop(bool reboots) {
    TTestBasicRuntime runtime;
    TTester::Setup(runtime);
    auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();

    TActorId sender = runtime.AllocateEdgeActor();
    CreateTestBootstrapper(runtime,
                           CreateTestTabletInfo(TTestTxConfig::TxTablet0, TTabletTypes::ColumnShard),
                           &CreateColumnShard);

    TDispatchOptions options;
    options.FinalEvents.push_back(TDispatchOptions::TFinalEventCondition(TEvTablet::EvBoot));
    runtime.DispatchEvents(options);

    //

    ui64 writeId = 0;
    ui64 tableId = 1;
    ui64 txId = 100;

    auto planStep = SetupSchema(runtime, sender, tableId, TestTableDescription(), "none", ++txId);
    TString data1 = MakeTestBlob({0, PORTION_ROWS}, testYdbSchema);
    UNIT_ASSERT(data1.size() > NColumnShard::TLimits::MIN_BYTES_TO_INSERT);
    UNIT_ASSERT(data1.size() < 7 * 1024 * 1024);

    TString data2 = MakeTestBlob({0, 100}, testYdbSchema);
    UNIT_ASSERT(data2.size() < NColumnShard::TLimits::MIN_BYTES_TO_INSERT);

    // Write into index
    std::vector<ui64> writeIds;
    UNIT_ASSERT(WriteData(runtime, sender, ++writeId, tableId, data1, testYdbSchema, true, &writeIds));
    planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
    PlanCommit(runtime, sender, planStep, txId);

    // Write into InsertTable
    writeIds.clear();
    UNIT_ASSERT(WriteData(runtime, sender, ++writeId, tableId, data2, testYdbSchema, true, &writeIds));
    planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
    PlanCommit(runtime, sender, planStep, txId);

    if (reboots) {
        RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
    }

    // Drop table
    planStep = SetupSchema(runtime, sender, TTestSchema::DropTableTxBody(tableId, 2), ++txId);

    if (reboots) {
        RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
    }

    TAutoPtr<IEventHandle> handle;
    {
        TShardReader reader(runtime, TTestTxConfig::TxTablet0, tableId, NOlap::TSnapshot(planStep, Max<ui64>()));
        reader.SetReplyColumnIds(TTestSchema::GetColumnIds(TTestSchema::YdbSchema(), { TTestSchema::DefaultTtlColumn }));
        auto rb = reader.ReadAll();
        UNIT_ASSERT(reader.IsCorrectlyFinished());
        UNIT_ASSERT(!rb || !rb->num_rows());
    }
}

void TestDropWriteRace() {
    TTestBasicRuntime runtime;
    TTester::Setup(runtime);
    auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();

    TActorId sender = runtime.AllocateEdgeActor();
    CreateTestBootstrapper(runtime,
                           CreateTestTabletInfo(TTestTxConfig::TxTablet0, TTabletTypes::ColumnShard),
                           &CreateColumnShard);

    TDispatchOptions options;
    options.FinalEvents.push_back(TDispatchOptions::TFinalEventCondition(TEvTablet::EvBoot));
    runtime.DispatchEvents(options);

    //

    ui64 tableId = 1;
    ui64 txId = 100;
    ui32 writeId = 0;

    NLongTxService::TLongTxId longTxId;
    UNIT_ASSERT(longTxId.ParseString("ydb://long-tx/01ezvvxjdk2hd4vdgjs68knvp8?node_id=1"));

    auto planStep = SetupSchema(runtime, sender, tableId, TestTableDescription(), "none", ++txId);
    TString data = MakeTestBlob({0, 100}, testYdbSchema);
    UNIT_ASSERT(data.size() < NColumnShard::TLimits::MIN_BYTES_TO_INSERT);

    // Write into InsertTable
    ++txId;
    AFL_VERIFY(WriteData(runtime, sender, ++writeId, tableId, data, testYdbSchema));
    planStep = ProposeCommit(runtime, sender, txId, { writeId });
    const auto commitTxId = txId;

    // Drop table
    planStep = ProposeSchemaTx(runtime, sender, TTestSchema::DropTableTxBody(tableId, 2), ++txId);
    PlanSchemaTx(runtime, sender, NOlap::TSnapshot(planStep, txId));

    // Plan commit
    PlanCommit(runtime, sender, planStep + 1, commitTxId);
}

void TestCompaction(std::optional<ui32> numWrites = {}) {
    TTestBasicRuntime runtime;
    TTester::Setup(runtime);
    auto csControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<NOlap::TWaitCompactionController>();

    TActorId sender = runtime.AllocateEdgeActor();
    CreateTestBootstrapper(runtime,
                        CreateTestTabletInfo(TTestTxConfig::TxTablet0, TTabletTypes::ColumnShard),
                        &CreateColumnShard);

    TDispatchOptions options;
    options.FinalEvents.push_back(TDispatchOptions::TFinalEventCondition(TEvTablet::EvBoot));
    runtime.DispatchEvents(options);

    // Create table
    ui64 writeId = 0;
    ui64 tableId = 1;
    ui64 txId = 100;

    auto planStep = SetupSchema(runtime, sender, tableId, TestTableDescription(), "none", ++txId);
    // Set tiering

    ui64 ts = 1620000000;
    TInstant now = TAppData::TimeProvider->Now();
    TDuration allow = TDuration::Seconds(now.Seconds() - ts + 3600);
    TDuration disallow = TDuration::Seconds(now.Seconds() - ts - 3600);

    TTestSchema::TTableSpecials spec;
    spec.SetTtlColumn("timestamp");
    spec.Tiers.emplace_back(TTestSchema::TStorageTier("hot").SetTtlColumn("timestamp"));
    spec.Tiers.back().EvictAfter = disallow;
    spec.Tiers.emplace_back(TTestSchema::TStorageTier("cold").SetTtlColumn("timestamp"));
    spec.Tiers.back().EvictAfter = allow;
    spec.Tiers.back().S3 = TTestSchema::TStorageTier::FakeS3();

    planStep = SetupSchema(runtime, sender, TTestSchema::AlterTableTxBody(tableId, 1, testYdbSchema, testYdbPk, spec), ++txId);
    csControllerGuard->OverrideTierConfigs(runtime, sender, TTestSchema::BuildSnapshot(spec));

    // Writes

    std::vector<TString> blobs = MakeData({ts}, PORTION_ROWS, 0, spec.TtlColumn);
    const TString& triggerData = blobs[0];
    UNIT_ASSERT(triggerData.size() > NColumnShard::TLimits::MIN_BYTES_TO_INSERT);
    UNIT_ASSERT(triggerData.size() < NColumnShard::TLimits::GetMaxBlobSize());

    if (!numWrites) {
        numWrites = NOlap::TCompactionLimits().GranuleOverloadSize / triggerData.size();
    }

    ++txId;
    for (ui32 i = 0; i < *numWrites; ++i, ++writeId, ++txId) {
        std::vector<ui64> writeIds;
        UNIT_ASSERT(WriteData(runtime, sender, writeId, tableId, triggerData, testYdbSchema, true, &writeIds));

        planStep = ProposeCommit(runtime, sender, txId, writeIds);
        PlanCommit(runtime, sender, planStep, txId);

    }
}

}

namespace NColumnShard {
extern bool gAllowLogBatchingDefaultValue;
}

Y_UNIT_TEST_SUITE(TColumnShardTestSchema) {

    Y_UNIT_TEST(CreateTable) {
        ui64 tableId = 1;

        std::vector<TTypeId> intTypes = {
            NTypeIds::Timestamp,
            NTypeIds::Int8,
            NTypeIds::Int16,
            NTypeIds::Int32,
            NTypeIds::Int64,
            NTypeIds::Uint8,
            NTypeIds::Uint16,
            NTypeIds::Uint32,
            NTypeIds::Uint64,
            NTypeIds::Date,
            NTypeIds::Datetime,
            NTypeIds::Date32,
            NTypeIds::Datetime64,
            NTypeIds::Timestamp64,
            NTypeIds::Interval64
        };

        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();

        using namespace NTxUT;
        CreateTestBootstrapper(runtime, CreateTestTabletInfo(TTestTxConfig::TxTablet0, TTabletTypes::ColumnShard), &CreateColumnShard);

        TDispatchOptions options;
        options.FinalEvents.push_back(TDispatchOptions::TFinalEventCondition(TEvTablet::EvBoot));
        runtime.DispatchEvents(options);

        TActorId sender = runtime.AllocateEdgeActor();

        auto schema = TTestSchema::YdbSchema(NArrow::NTest::TTestColumn("k0", TTypeInfo(NTypeIds::Timestamp)));
        auto pk = NArrow::NTest::TTestColumn::CropSchema(schema, 4);

        TPlanStep planStep;
        ui64 txId = 100;
        ui64 generation = 0;

        planStep = SetupSchema(runtime, sender, TTestSchema::CreateInitShardTxBody(tableId++, schema, pk), txId++);
        for (auto& ydbType : intTypes) {
            schema[0].SetType(TTypeInfo(ydbType));
            pk[0].SetType(TTypeInfo(ydbType));
            auto txBody = TTestSchema::CreateTableTxBody(tableId++, schema, pk, {}, ++generation);
            planStep = SetupSchema(runtime, sender, txBody, txId++);
        }

        // TODO: support float types
        std::vector<TTypeId> floatTypes = {
            NTypeIds::Float,
            NTypeIds::Double
        };

        for (auto& ydbType : floatTypes) {
            schema[0].SetType(TTypeInfo(ydbType));
            pk[0].SetType(TTypeInfo(ydbType));
            auto txBody = TTestSchema::CreateTableTxBody(tableId++, schema, pk, {}, ++generation);
            ProposeSchemaTxFail(runtime, sender, txBody, txId++);
        }

        std::vector<TTypeId> strTypes = {
            NTypeIds::String,
            NTypeIds::Utf8
        };

        for (auto& ydbType : strTypes) {
            schema[0].SetType(TTypeInfo(ydbType));
            pk[0].SetType(TTypeInfo(ydbType));
            auto txBody = TTestSchema::CreateTableTxBody(tableId++, schema, pk, {}, ++generation);
            planStep = SetupSchema(runtime, sender, txBody, txId++);
        }

        std::vector<TTypeId> xsonTypes = {
            NTypeIds::Yson,
            NTypeIds::Json,
            NTypeIds::JsonDocument
        };

        for (auto& ydbType : xsonTypes) {
            schema[0].SetType(TTypeInfo(ydbType));
            pk[0].SetType(TTypeInfo(ydbType));
            auto txBody = TTestSchema::CreateTableTxBody(tableId++, schema, pk, {}, ++generation);
            ProposeSchemaTxFail(runtime, sender, txBody, txId++);
        }
    }

    Y_UNIT_TEST_OCTO(TTL, Reboot, Internal, FirstPkColumn) {
        for (auto typeId : { NTypeIds::Timestamp, NTypeIds::Datetime, NTypeIds::Date, NTypeIds::Uint32, NTypeIds::Uint64 }) {
            Cerr << "Running TestTtl ttlColumnType=" << NKikimr::NScheme::TypeName(typeId) << Endl;
            TestTtl(Reboot, Internal, FirstPkColumn, typeId);
        }
    }




    // TODO: EnableOneTierAfterTtl, EnableTtlAfterOneTier

    Y_UNIT_TEST(HotTiers) {
        TestTwoHotTiers(false, false);
    }

    Y_UNIT_TEST(RebootHotTiers) {
        TestTwoHotTiers(true, false);
    }

    Y_UNIT_TEST(HotTiersWithStat) {
        TestTwoHotTiers(false, false);
    }

    Y_UNIT_TEST(RebootHotTiersWithStat) {
        TestTwoHotTiers(true, false);
    }

    Y_UNIT_TEST(HotTiersRevCompression) {
        TestTwoHotTiers(false, false, EInitialEviction::None);
    }

    Y_UNIT_TEST(RebootHotTiersRevCompression) {
        TestTwoHotTiers(true, false, EInitialEviction::None);
    }

    Y_UNIT_TEST(HotTiersTtl) {
        NColumnShard::gAllowLogBatchingDefaultValue = false;
        TestTwoHotTiers(false, true);
    }

    Y_UNIT_TEST(RebootHotTiersTtl) {
        NColumnShard::gAllowLogBatchingDefaultValue = false;
        TestTwoHotTiers(true, true);
    }

    Y_UNIT_TEST(HotTiersTtlWithStat) {
        NColumnShard::gAllowLogBatchingDefaultValue = false;
        TestTwoHotTiers(false, true);
    }

    Y_UNIT_TEST(RebootHotTiersTtlWithStat) {
        NColumnShard::gAllowLogBatchingDefaultValue = false;
        TestTwoHotTiers(true, true);
    }

    Y_UNIT_TEST(HotTiersAfterTtl) {
        TestTwoHotTiers(false, false, EInitialEviction::Ttl);
    }

    Y_UNIT_TEST(RebootHotTiersAfterTtl) {
        TestTwoHotTiers(true, false, EInitialEviction::Ttl);
    }

    // TODO: EnableTtlAfterHotTiers

    Y_UNIT_TEST(ColdTiers) {
        TestHotAndColdTiers(false, EInitialEviction::Tiering);
    }

    Y_UNIT_TEST(RebootColdTiers) {
        //NColumnShard::gAllowLogBatchingDefaultValue = false;
        TestHotAndColdTiers(true, EInitialEviction::Tiering);
    }

    Y_UNIT_TEST(ColdTiersWithStat) {
        TestHotAndColdTiers(false, EInitialEviction::Tiering);
    }

    Y_UNIT_TEST(RebootColdTiersWithStat) {
        //NColumnShard::gAllowLogBatchingDefaultValue = false;
        TestHotAndColdTiers(true, EInitialEviction::Tiering);
    }

    Y_UNIT_TEST(EnableColdTiersAfterNoEviction) {
        TestHotAndColdTiers(false, EInitialEviction::None);
    }

    Y_UNIT_TEST(RebootEnableColdTiersAfterNoEviction) {
        TestHotAndColdTiers(true, EInitialEviction::None);
    }

    Y_UNIT_TEST(EnableColdTiersAfterTtl) {
        TestHotAndColdTiers(false, EInitialEviction::Ttl);
    }

    Y_UNIT_TEST(RebootEnableColdTiersAfterTtl) {
        TestHotAndColdTiers(true, EInitialEviction::Ttl);
    }

    Y_UNIT_TEST(OneColdTier) {
        TestExport(false);
    }

    Y_UNIT_TEST(RebootOneColdTier) {
        TestExport(true);
    }

    Y_UNIT_TEST(ExportAfterFail) {
        TestExport(false, TExportTestOpts{.Misconfig = 1});
    }

    Y_UNIT_TEST(RebootExportAfterFail) {
        TestExport(true, TExportTestOpts{.Misconfig = 1});
    }

    Y_UNIT_TEST(ForgetAfterFail) {
        TestExport(false, TExportTestOpts{.Misconfig = 2});
    }

    Y_UNIT_TEST(RebootForgetAfterFail) {
        TestExport(true, TExportTestOpts{.Misconfig = 2});
    }

    Y_UNIT_TEST(ExportWithLostAnswer) {
        TestExport(false, TExportTestOpts{.Loss = 1});
    }

    Y_UNIT_TEST(RebootExportWithLostAnswer) {
        TestExport(true, TExportTestOpts{.Loss = 1});
    }

    Y_UNIT_TEST(ForgetWithLostAnswer) {
        TestExport(false, TExportTestOpts{.Loss = 2});
    }

    Y_UNIT_TEST(RebootForgetWithLostAnswer) {
        TestExport(true, TExportTestOpts{.Loss = 2});
    }
#if 0
    Y_UNIT_TEST(RebootReadNoTier) {
        TestExport(true, TExportTestOpts{.NoTier = 3});
    }
#endif
    // TODO: LastTierBorderIsTtl = false
    // TODO: AlterTierBorderAfterExport

    Y_UNIT_TEST(ColdCompactionSmoke) {
        TestCompaction();
    }

    Y_UNIT_TEST(Drop) {
        TestDrop(false);
    }

    Y_UNIT_TEST(RebootDrop) {
        TestDrop(true);
    }

    Y_UNIT_TEST(DropWriteRace) {
        TestDropWriteRace();
    }
}

}
