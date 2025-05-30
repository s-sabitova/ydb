#include "actorsystem.h"
#include "executor_pool_basic.h"
#include "hfunc.h"
#include "scheduler_basic.h"

#include <ydb/library/actors/util/should_continue.h>

#include <library/cpp/testing/unittest/registar.h>

using namespace NActors;

////////////////////////////////////////////////////////////////////////////////

struct TEvMsg : public NActors::TEventLocal<TEvMsg, 10347> {
};

////////////////////////////////////////////////////////////////////////////////

class TTestSenderActor : public IActorCallback {
private:
    using EActivityType = IActor::EActivityType ;
    using EActorActivity = IActor::EActorActivity;

private:
    TAtomic Counter;
    TActorId Receiver;

    std::function<void(void)> Action;

public:
    TTestSenderActor(std::function<void(void)> action = [](){},
                     EActivityType activityType =  EActorActivity::OTHER)
        : IActorCallback(static_cast<TReceiveFunc>(&TTestSenderActor::Execute), activityType)
        , Action(action)
    {}

    void Start(TActorId receiver, size_t count)
    {
        AtomicSet(Counter, count);
        Receiver = receiver;
    }

    void Stop() {
        while (true) {
            if (GetCounter() == 0) {
                break;
            }

            Sleep(TDuration::MilliSeconds(1));
        }
    }

    size_t GetCounter() const {
        return AtomicGet(Counter);
    }

private:
    STFUNC(Execute)
    {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvMsg, Handle);
        }
    }

    void Handle(TEvMsg::TPtr &ev)
    {
        Y_UNUSED(ev);
        Action();
        TAtomicBase count = AtomicDecrement(Counter);
        Y_ABORT_UNLESS(count != Max<TAtomicBase>());
        if (count) {
            Send(Receiver, new TEvMsg());
        }
    }
};

THolder<TActorSystemSetup> GetActorSystemSetup(TBasicExecutorPool* pool)
{
    auto setup = MakeHolder<NActors::TActorSystemSetup>();
    setup->NodeId = 1;
    setup->ExecutorsCount = 1;
    setup->Executors.Reset(new TAutoPtr<NActors::IExecutorPool>[1]);
    setup->Executors[0] = pool;
    setup->Scheduler = new TBasicSchedulerThread(NActors::TSchedulerConfig(512, 0));
    return setup;
}

Y_UNIT_TEST_SUITE(BasicExecutorPool) {

    Y_UNIT_TEST(CheckCompleteOne) {
        const size_t size = 4;
        const size_t msgCount = 1e4;
        TBasicExecutorPool* executorPool = new TBasicExecutorPool(0, size, 50);

        auto setup = GetActorSystemSetup(executorPool);
        TActorSystem actorSystem(setup);
        actorSystem.Start();

        auto begin = TInstant::Now();

        auto actor = new TTestSenderActor();
        auto actorId = actorSystem.Register(actor);
        actor->Start(actor->SelfId(), msgCount);
        actorSystem.Send(actorId, new TEvMsg());

        while (actor->GetCounter()) {
            auto now = TInstant::Now();
            UNIT_ASSERT_C(now - begin < TDuration::Seconds(5), "Counter is " << actor->GetCounter());

            Sleep(TDuration::MilliSeconds(1));
        }
    }

    Y_UNIT_TEST(CheckCompleteAll) {
        const size_t size = 4;
        const size_t msgCount = 1e4;
        TBasicExecutorPool* executorPool = new TBasicExecutorPool(0, size, 50);

        auto setup = GetActorSystemSetup(executorPool);
        TActorSystem actorSystem(setup);
        actorSystem.Start();

        auto begin = TInstant::Now();

        TTestSenderActor* actors[size];
        TActorId actorIds[size];

        for (size_t i = 0; i < size; ++i) {
            actors[i] = new TTestSenderActor();
            actorIds[i] = actorSystem.Register(actors[i]);
        }
        for (size_t i = 0; i < size; ++i) {
            actors[i]->Start(actors[i]->SelfId(), msgCount);
        }
        for (size_t i = 0; i < size; ++i) {
            actorSystem.Send(actorIds[i], new TEvMsg());
        }


        while (true) {
            size_t maxCounter = 0;
            for (size_t i = 0; i < size; ++i) {
                maxCounter = Max(maxCounter, actors[i]->GetCounter());
            }

            if (maxCounter == 0) {
                break;
            }

            auto now = TInstant::Now();
            UNIT_ASSERT_C(now - begin < TDuration::Seconds(5), "Max counter is " << maxCounter);

            Sleep(TDuration::MilliSeconds(1));
        }
    }

    Y_UNIT_TEST(CheckCompleteOver) {
        const size_t size = 4;
        const size_t actorsCount = size * 2;
        const size_t msgCount = 1e4;
        TBasicExecutorPool* executorPool = new TBasicExecutorPool(0, size, 50);

        auto setup = GetActorSystemSetup(executorPool);
        TActorSystem actorSystem(setup);
        actorSystem.Start();

        auto begin = TInstant::Now();

        TTestSenderActor* actors[actorsCount];
        TActorId actorIds[actorsCount];

        for (size_t i = 0; i < actorsCount; ++i) {
            actors[i] = new TTestSenderActor();
            actorIds[i] = actorSystem.Register(actors[i]);
        }
        for (size_t i = 0; i < actorsCount; ++i) {
            actors[i]->Start(actors[i]->SelfId(), msgCount);
        }
        for (size_t i = 0; i < actorsCount; ++i) {
            actorSystem.Send(actorIds[i], new TEvMsg());
        }


        while (true) {
            size_t maxCounter = 0;
            for (size_t i = 0; i < actorsCount; ++i) {
                maxCounter = Max(maxCounter, actors[i]->GetCounter());
            }

            if (maxCounter == 0) {
                break;
            }

            auto now = TInstant::Now();
            UNIT_ASSERT_C(now - begin < TDuration::Seconds(5), "Max counter is " << maxCounter);

            Sleep(TDuration::MilliSeconds(1));
        }
    }

    Y_UNIT_TEST(CheckCompleteRoundRobinOver) {
        const size_t size = 4;
        const size_t actorsCount = size * 2;
        const size_t msgCount = 1e2;
        TBasicExecutorPool* executorPool = new TBasicExecutorPool(0, size, 50);

        auto setup = GetActorSystemSetup(executorPool);
        TActorSystem actorSystem(setup);
        actorSystem.Start();

        auto begin = TInstant::Now();

        TTestSenderActor* actors[actorsCount];
        TActorId actorIds[actorsCount];

        for (size_t i = 0; i < actorsCount; ++i) {
            actors[i] = new TTestSenderActor();
            actorIds[i] = actorSystem.Register(actors[i]);
        }
        for (size_t i = 0; i < actorsCount; ++i) {
            actors[i]->Start(actorIds[(i + 1) % actorsCount], msgCount);
        }
        for (size_t i = 0; i < actorsCount; ++i) {
            actorSystem.Send(actorIds[i], new TEvMsg());
        }

        while (true) {
            size_t maxCounter = 0;
            for (size_t i = 0; i < actorsCount; ++i) {
                maxCounter = Max(maxCounter, actors[i]->GetCounter());
            }

            if (maxCounter == 0) {
                break;
            }

            auto now = TInstant::Now();
            UNIT_ASSERT_C(now - begin < TDuration::Seconds(5), "Max counter is " << maxCounter);

            Sleep(TDuration::MilliSeconds(1));
        }
    }

    Y_UNIT_TEST(CheckStats) {
        const size_t size = 4;
        const size_t msgCount = 5e3;
        TBasicExecutorPool* executorPool = new TBasicExecutorPool(0, size, 50);

        auto setup = GetActorSystemSetup(executorPool);
        TActorSystem actorSystem(setup);
        actorSystem.Start();

        auto begin = TInstant::Now();

        auto load = [&]() {
            auto actor = new TTestSenderActor();
            auto actorId = actorSystem.Register(actor);
            actor->Start(actor->SelfId(), msgCount);
            actorSystem.Send(actorId, new TEvMsg());

            while (actor->GetCounter()) {
                auto now = TInstant::Now();
                UNIT_ASSERT_C(now - begin < TDuration::Seconds(5), "Counter is " << actor->GetCounter());

                Sleep(TDuration::MilliSeconds(1));
            }
        };

        load();
        Sleep(TDuration::MilliSeconds(10));
        load();

        TVector<TExecutorThreadStats> stats;
        TExecutorPoolStats poolStats;
        actorSystem.GetPoolStats(0, poolStats, stats);
        // Sum all per-thread counters into the 0th element
        for (ui32 idx = 1; idx < stats.size(); ++idx) {
            stats[0].Aggregate(stats[idx]);
        }

        UNIT_ASSERT_VALUES_EQUAL(stats[0].SentEvents, 2 * msgCount - 2);
        UNIT_ASSERT_VALUES_EQUAL(stats[0].ReceivedEvents, 2 * msgCount);
        UNIT_ASSERT_VALUES_EQUAL(stats[0].PreemptedEvents, 0);
        UNIT_ASSERT_VALUES_EQUAL(stats[0].NonDeliveredEvents, 0);
        UNIT_ASSERT_VALUES_EQUAL(stats[0].EmptyMailboxActivation, 0);
        //UNIT_ASSERT_VALUES_EQUAL(stats[0].CpuUs, 0); // depends on total duration of test, so undefined
        UNIT_ASSERT(stats[0].ElapsedTicks > 0);
        UNIT_ASSERT(stats[0].ParkedTicks > 0);
        UNIT_ASSERT(stats[0].ActivationTimeHistogram.TotalSamples >= 2 * msgCount / TBasicExecutorPoolConfig::DEFAULT_EVENTS_PER_MAILBOX);
        UNIT_ASSERT_VALUES_EQUAL(stats[0].EventDeliveryTimeHistogram.TotalSamples, 2 * msgCount);
        UNIT_ASSERT_VALUES_EQUAL(stats[0].EventProcessingCountHistogram.TotalSamples, 2 * msgCount);
        UNIT_ASSERT(stats[0].EventProcessingTimeHistogram.TotalSamples > 0);
        UNIT_ASSERT(stats[0].ElapsedTicksByActivity[NActors::TActorTypeOperator::GetOtherActivityIndex()] > 0);
        UNIT_ASSERT_VALUES_EQUAL(stats[0].ReceivedEventsByActivity[NActors::TActorTypeOperator::GetOtherActivityIndex()], 2 * msgCount);
        UNIT_ASSERT_VALUES_EQUAL(stats[0].ActorsAliveByActivity[NActors::TActorTypeOperator::GetOtherActivityIndex()], 2);
        UNIT_ASSERT_VALUES_EQUAL(stats[0].ScheduledEventsByActivity[NActors::TActorTypeOperator::GetOtherActivityIndex()], 0);
        UNIT_ASSERT_VALUES_EQUAL(stats[0].PoolActorRegistrations, 2);
        UNIT_ASSERT_VALUES_EQUAL(stats[0].PoolDestroyedActors, 0);
        UNIT_ASSERT_VALUES_EQUAL(stats[0].PoolAllocatedMailboxes, 4096); // one line
        UNIT_ASSERT(stats[0].MailboxPushedOutByTime + stats[0].MailboxPushedOutByEventCount >= 2 * msgCount / TBasicExecutorPoolConfig::DEFAULT_EVENTS_PER_MAILBOX);
        UNIT_ASSERT_VALUES_EQUAL(stats[0].MailboxPushedOutBySoftPreemption, 0);
    }
}

Y_UNIT_TEST_SUITE(ChangingThreadsCountInBasicExecutorPool) {

    struct TMockState {
        void ActorDo() {}
    };

    struct TTestActors {
        const size_t Count;
        TArrayHolder<TTestSenderActor*> Actors;
        TArrayHolder<TActorId> ActorIds;

        TTestActors(size_t count)
            : Count(count)
            , Actors(new TTestSenderActor*[count])
            , ActorIds(new TActorId[count])
        { }

        void Start(TActorSystem &actorSystem, size_t msgCount) {
            for (size_t i = 0; i < Count; ++i) {
                Actors[i]->Start(Actors[i]->SelfId(), msgCount);
            }
            for (size_t i = 0; i < Count; ++i) {
                actorSystem.Send(ActorIds[i], new TEvMsg());
            }
        }

        void Stop() {
            for (size_t i = 0; i < Count; ++i) {
                Actors[i]->Stop();
            }
        }
    };

    template <typename TState = TMockState>
    struct TTestCtx {
        const size_t MaxThreadCount;
        const size_t SendingMessageCount;
        std::unique_ptr<TBasicExecutorPool> ExecutorPool;
        std::unique_ptr<TActorSystem> ActorSystem;

        TState State;

        void Init(size_t, size_t) {
            TBasicExecutorPoolConfig config;
            config.MaxThreadCount = MaxThreadCount;
            config.Threads = MaxThreadCount;
            config.MinThreadCount = 1;
            config.DefaultThreadCount = 1;
            config.EventsPerMailbox = 50;
            ExecutorPool.reset(new TBasicExecutorPool(config, nullptr, nullptr));
            THolder<TActorSystemSetup> setup = GetActorSystemSetup(ExecutorPool.get());
            ActorSystem.reset(new TActorSystem(setup));
        }

        TTestCtx(size_t maxThreadCount, size_t sendingMessageCount)
            : MaxThreadCount(maxThreadCount)
            , SendingMessageCount(sendingMessageCount)
        {
            Init(maxThreadCount, sendingMessageCount);
        }

        TTestCtx(size_t maxThreadCount, size_t sendingMessageCount, const TState &state)
            : MaxThreadCount(maxThreadCount)
            , SendingMessageCount(sendingMessageCount)
            , State(state)
        {
            Init(maxThreadCount, sendingMessageCount);
        }

        ~TTestCtx() {
            ExecutorPool.release();
        }

        TTestActors RegisterCheckActors(size_t actorCount) {
            TTestActors res(actorCount);
            for (size_t i = 0; i < actorCount; ++i) {
                res.Actors[i] = new TTestSenderActor([&] {
                    State.ActorDo();
                });
                res.ActorIds[i] = ActorSystem->Register(res.Actors[i]);
            }
            return res;
        }
    };

    struct TCheckingInFlightState {
        TAtomic ExpectedMaximum = 0;
        TAtomic CurrentInFlight = 0;

        void ActorStartProcessing() {
            ui32 inFlight = AtomicIncrement(CurrentInFlight);
            ui32 maximum = AtomicGet(ExpectedMaximum);
            if (maximum) {
                UNIT_ASSERT_C(inFlight <= maximum, "inFlight# " << inFlight << " maximum# " << maximum);
            }
        }

        void ActorStopProcessing() {
            AtomicDecrement(CurrentInFlight);
        }

        void ActorDo() {
            ActorStartProcessing();
            NanoSleep(1'000'000);
            ActorStopProcessing();
        }
    };

    Y_UNIT_TEST(DecreaseIncreaseThreadCount) {
        const size_t msgCount = 1e2;
        const size_t size = 4;
        const size_t testCount = 2;
        TTestCtx<TCheckingInFlightState> ctx(size, msgCount);
        ctx.ActorSystem->Start();

        TTestActors testActors = ctx.RegisterCheckActors(size);

        const size_t N = 6;
        const size_t threadsCounts[N] = { 1, 3, 2, 3, 1, 4 };
        for (ui32 idx = 0; idx < 4 * N; ++idx) {
            size_t currentThreadCount = threadsCounts[idx % N];
            ctx.ExecutorPool->SetFullThreadCount(currentThreadCount);
            AtomicSet(ctx.State.ExpectedMaximum, currentThreadCount);

            for (size_t testIdx = 0; testIdx < testCount; ++testIdx) {
                testActors.Start(*ctx.ActorSystem, msgCount);
                Sleep(TDuration::MilliSeconds(100));
                testActors.Stop();
            }
            Sleep(TDuration::MilliSeconds(10));
        }
        ctx.ActorSystem->Stop();
    }

    Y_UNIT_TEST(ContiniousChangingThreadCount) {
        const size_t msgCount = 1e2;
        const size_t size = 4;

        auto begin = TInstant::Now();
        TTestCtx<TCheckingInFlightState> ctx(size, msgCount, TCheckingInFlightState{msgCount});
        ctx.ActorSystem->Start();
        TTestActors testActors = ctx.RegisterCheckActors(size);

        testActors.Start(*ctx.ActorSystem, msgCount);

        const size_t N = 6;
        const size_t threadsCouns[N] = { 1, 3, 2, 3, 1, 4 };

        ui64 counter = 0;

        TTestSenderActor* changerActor = new TTestSenderActor([&]{
            ctx.State.ActorStartProcessing();
            AtomicSet(ctx.State.ExpectedMaximum, 0);
            ctx.ExecutorPool->SetFullThreadCount(threadsCouns[counter]);
            NanoSleep(10'000'000);
            AtomicSet(ctx.State.ExpectedMaximum, threadsCouns[counter]);
            counter++;
            if (counter == N) {
                counter = 0;
            }
            ctx.State.ActorStopProcessing();
        });
        TActorId changerActorId = ctx.ActorSystem->Register(changerActor);
        changerActor->Start(changerActorId, msgCount);
        ctx.ActorSystem->Send(changerActorId, new TEvMsg());

        while (true) {
            size_t maxCounter = 0;
            for (size_t i = 0; i < size; ++i) {
                maxCounter = Max(maxCounter, testActors.Actors[i]->GetCounter());
            }
            if (maxCounter == 0) {
                break;
            }
            auto now = TInstant::Now();
            UNIT_ASSERT_C(now - begin < TDuration::Seconds(5), "Max counter is " << maxCounter);
            Sleep(TDuration::MilliSeconds(1));
        }

        changerActor->Stop();
        ctx.ActorSystem->Stop();
    }
}
