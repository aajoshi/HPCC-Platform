/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#include "jiface.hpp"

#include "eclhelper.hpp"
#include "eclrtl_imp.hpp"
#include "thdemonserver.hpp"
#include "thcompressutil.hpp"
#include "thmem.hpp"

#include "thloop.ipp"

#define SYNC_TIMEOUT (5*60*1000)

class CLoopActivityMasterBase : public CMasterActivity
{
protected:
    rtlRowBuilder extractBuilder;
    CGraphBase *loopGraph = nullptr;
    unsigned emptyIterations = 0;
    unsigned maxEmptyLoopIterations = 1000;
    Owned<CThorStats> loopCounterProgress;
    bool syncIterations = false;
    bool loopIsInGlobalGraph = false;
    mptag_t syncMpTag = TAG_NULL;

    bool sync(unsigned loopCounter)
    {
        unsigned loopEnds = 0;
        unsigned nodes = queryJob().querySlaves();
        unsigned n = nodes;
        bool allEmptyIterations = true;
        CMessageBuffer msg;
        while (n--) // a barrier really
        {
            for (;;)
            {
                rank_t sender;
                if (receiveMsg(msg, RANK_ALL, syncMpTag, &sender, SYNC_TIMEOUT))
                    break;
                if (abortSoon)
                    return true; // NB: returning true, denotes end of loop
                ActPrintLog("Still waiting for %d slaves to synchronize global loop", n+1);
            }
            unsigned slaveLoopCounterReq, slaveEmptyIterations;
            msg.read(slaveLoopCounterReq);
            msg.read(slaveEmptyIterations);
            if (0 == slaveLoopCounterReq) // signals end
            {
                ++loopEnds;
                if (loopEnds == nodes)
                    break;
            }
            else
                assertex(slaveLoopCounterReq == loopCounter); // sanity check
            if (0 == slaveEmptyIterations) // either 1st or has been reset, i.e. non-empty
                allEmptyIterations = false;
        }
        bool final = loopEnds == nodes; // final
        msg.clear();
        if (allEmptyIterations)
            emptyIterations++;
        else
            emptyIterations = 0;
        bool ok = emptyIterations <= maxEmptyLoopIterations;
        msg.append(ok && !final); // This is to tell slave whether it should continue or not
        n = nodes;
        while (n--) // a barrier really
            queryJobChannel().queryJobComm().send(msg, n+1, syncMpTag, LONGTIMEOUT);

        if (!ok)
            throw MakeActivityException(this, 0, "Executed LOOP with empty input and output %u times", emptyIterations);

        return final;
    }
public:
    CLoopActivityMasterBase(CMasterGraphElement *info) : CMasterActivity(info)
    {
        loopCounterProgress.setown(new CThorStats(queryJob(), StNumIterations));
        maxEmptyLoopIterations = getOptUInt(THOROPT_LOOP_MAX_EMPTY, 1000);
        loopIsInGlobalGraph = container.queryOwner().isGlobal();
        loopGraph = nullptr;
    }
    ~CLoopActivityMasterBase()
    {
        if (TAG_NULL != syncMpTag)
            queryJob().freeMPTag(syncMpTag);
    }
    virtual bool fireException(IException *e) override
    {
        EXCLOG(e, "Loop master passed exception, aborting loop graph(s)");
        try
        {
            loopGraph->abort(e);
        }
        catch (IException *e)
        {
            EXCLOG(e, "Exception whilst aborting loop graphs");
            e->Release();
        }
        return CMasterActivity::fireException(e);
    }
    virtual void init() override
    {
        CMasterActivity::init();
        loopGraph = queryContainer().queryLoopGraph()->queryGraph();
        syncIterations = !loopGraph->isLocalOnly();
        if (loopIsInGlobalGraph)
            syncMpTag = queryJob().allocateMPTag();
    }
    virtual void process() override
    {
        CMasterActivity::process();
        emptyIterations = 0;
    }
    virtual void serializeSlaveData(MemoryBuffer &dst, unsigned slave) override
    {
        if (loopIsInGlobalGraph)
            serializeMPtag(dst, syncMpTag);
    }
    virtual void slaveDone(size32_t slaveIdx, MemoryBuffer &mb) override
    {
        CMasterGraph *graph = (CMasterGraph *)loopGraph;
        graph->handleSlaveDone(slaveIdx, mb);
    }
    virtual void abort() override
    {
        CMasterActivity::abort();
        if (loopIsInGlobalGraph)
            cancelReceiveMsg(RANK_ALL, syncMpTag);
    }
    virtual void deserializeStats(unsigned node, MemoryBuffer &mb) override
    {
        CMasterActivity::deserializeStats(node, mb);
        unsigned loopCounter;
        mb.read(loopCounter);
        loopCounterProgress->set(node, loopCounter);
    }
    virtual void getActivityStats(IStatisticGatherer & stats) override
    {
        CMasterActivity::getActivityStats(stats);
        loopCounterProgress->getStats(stats, false);
    }
};


class CLoopActivityMaster : public CLoopActivityMasterBase
{
    IHThorLoopArg *helper;
    unsigned flags;
    Owned<IBarrier> barrier;

    void checkEmpty()
    {
        // similar to sync, but continiously listens for messages from slaves
        // slave only sends if above threashold, or if was at threshold and non empty
        // this routine is here to spot when all are whirling around processing nothing for > threshold
        Owned<IBitSet> emptyIterationSlaves = createThreadSafeBitSet();
        unsigned loopEnds = 0;
        unsigned nodes = queryJob().querySlaves();
        unsigned n = nodes;
        bool allEmptyIterations = true;
        CMessageBuffer msg;
        for (;;)
        {
            rank_t sender;
            if (!receiveMsg(msg, RANK_ALL, syncMpTag, &sender, LONGTIMEOUT))
                return;
            unsigned slaveLoopCounterReq, slaveEmptyIterations;
            msg.read(slaveLoopCounterReq);
            msg.read(slaveEmptyIterations);
            if (0 == slaveLoopCounterReq) // signals end
            {
                ++loopEnds;
                if (loopEnds == nodes)
                    break; // all done
            }
            bool overLimit = slaveEmptyIterations > maxEmptyLoopIterations;
            emptyIterationSlaves->set(sender-1, overLimit);
            if (emptyIterationSlaves->scan(0, 0) >= nodes) // all empty
                throw MakeActivityException(this, 0, "Executed LOOP with empty input and output > %d maxEmptyLoopIterations times on all nodes", maxEmptyLoopIterations);
        }
    }
public:
    CLoopActivityMaster(CMasterGraphElement *info) : CLoopActivityMasterBase(info)
    {
    }
    virtual void init() override
    {
        CLoopActivityMasterBase::init();
        helper = (IHThorLoopArg *) queryHelper();
        flags = helper->getFlags();
        if (TAKloopdataset == container.getKind())
            assertex(flags & IHThorLoopArg::LFnewloopagain);
        if (flags & IHThorLoopArg::LFnewloopagain)
        {
            if (loopIsInGlobalGraph)
            {
                mpTag = queryJob().allocateMPTag();
                barrier.setown(queryJobChannel().createBarrier(mpTag));
                syncIterations = true;
            }
        }
    }
    virtual void process() override
    {
        CLoopActivityMasterBase::process();
        if (!loopIsInGlobalGraph)
            return;

        if (syncIterations)
        {
            helper->createParentExtract(extractBuilder);
            unsigned loopCounter = 1;
            for (;;)
            {
                if (sync(loopCounter))
                    break;

                // NB: This is exactly the same as the slave implementation up until the execute().
                IThorBoundLoopGraph *boundGraph = queryContainer().queryLoopGraph();
                unsigned condLoopCounter = (flags & IHThorLoopArg::LFcounter) ? loopCounter : 0;
                unsigned loopAgain = (flags & IHThorLoopArg::LFnewloopagain) ? helper->loopAgainResult() : 0;
                ownedResults.setown(queryGraph().createThorGraphResults(3)); // will not be cleared until next sync
                // ensures remote results are available, via owning activity (i.e. this loop act)
                // so that when the master/slave result parts are fetched, it will retreive from the act, not the (already cleaed) graph localresults
                ownedResults->setOwner(container.queryId());

                boundGraph->prepareLoopResults(*this, ownedResults);
                if (condLoopCounter)
                    boundGraph->prepareCounterResult(*this, ownedResults, condLoopCounter, 2);
                if (loopAgain) // cannot be 0
                    boundGraph->prepareLoopAgainResult(*this, ownedResults, loopAgain);

                boundGraph->execute(*this, condLoopCounter, ownedResults, (IRowWriterMultiReader *)NULL, 0, extractBuilder.size(), extractBuilder.getbytes());
                ++loopCounter;
                if (barrier)
                {
                    if (!barrier->wait(false))
                        break;
                }
            }
        }
        else
            checkEmpty();
    }
    virtual void serializeSlaveData(MemoryBuffer &dst, unsigned slave) override
    {
        CLoopActivityMasterBase::serializeSlaveData(dst, slave);
        if (barrier)
            serializeMPtag(dst, mpTag);
    }
    virtual void abort() override
    {
        CLoopActivityMasterBase::abort();
        if (barrier)
            barrier->cancel();
    }
};

CActivityBase *createLoopActivityMaster(CMasterGraphElement *container)
{
    return new CLoopActivityMaster(container);
}

///////////

class CGraphLoopActivityMaster : public CLoopActivityMasterBase
{
public:
    CGraphLoopActivityMaster(CMasterGraphElement *info) : CLoopActivityMasterBase(info)
    {
    }
    virtual void init() override
    {
        CLoopActivityMasterBase::init();
        if (!syncIterations)
            return;
        IHThorGraphLoopArg *helper = (IHThorGraphLoopArg *) queryHelper();
        Owned<IThorGraphResults> results = queryGraph().createThorGraphResults(1);
        if (helper->getFlags() & IHThorGraphLoopArg::GLFcounter)
            queryContainer().queryLoopGraph()->prepareCounterResult(*this, results, 1, 0);
        loopGraph->setResults(results);
    }
    virtual void process() override
    {
        CLoopActivityMasterBase::process();
        if (!loopIsInGlobalGraph)
            return;

        IHThorGraphLoopArg *helper = (IHThorGraphLoopArg *) queryHelper();
        unsigned maxIterations = helper->numIterations();
        if ((int)maxIterations < 0) maxIterations = 0;
        Owned<IThorGraphResults> loopResults = queryGraph().createThorGraphResults(maxIterations);
        IThorResult *result = loopResults->createResult(*this, 0, this, mergeResultTypes(thorgraphresult_distributed, thorgraphresult_grouped));

        helper->createParentExtract(extractBuilder);

        unsigned loopCounter = 1;
        for (;;)
        {
            if (sync(loopCounter))
                break;
            queryContainer().queryLoopGraph()->execute(*this, (helper->getFlags() & IHThorGraphLoopArg::GLFcounter)?loopCounter:0, loopResults.get(), extractBuilder.size(), extractBuilder.getbytes());
            ++loopCounter;
        }
    }
};

CActivityBase *createGraphLoopActivityMaster(CMasterGraphElement *container)
{
    return new CGraphLoopActivityMaster(container);
}

///////////

class CLocalResultActivityMasterBase : public CMasterActivity
{
protected:
    Owned<IThorRowInterfaces> inputRowIf;

public:
    CLocalResultActivityMasterBase(CMasterGraphElement *info) : CMasterActivity(info)
    {
    }
    virtual void init() override
    {
        CMasterActivity::init();
        reset();
    }
    virtual void createResult() = 0;
    virtual void process() override
    {
        IHThorLocalResultWriteArg *helper = (IHThorLocalResultWriteArg *)queryHelper();
        inputRowIf.setown(createRowInterfaces(container.queryInput(0)->queryHelper()->queryOutputMeta()));
        createResult();
    }
};

class CLocalResultActivityMaster : public CLocalResultActivityMasterBase
{
public:
    CLocalResultActivityMaster(CMasterGraphElement *info) : CLocalResultActivityMasterBase(info)
    {
    }
    virtual void createResult() override
    {
        IHThorLocalResultWriteArg *helper = (IHThorLocalResultWriteArg *)queryHelper();
        CGraphBase *graph = container.queryResultsGraph();
        graph->createResult(*this, helper->querySequence(), this, mergeResultTypes(thorgraphresult_distributed, thorgraphresult_grouped)); // NB graph owns result
    }
};

CActivityBase *createLocalResultActivityMaster(CMasterGraphElement *container)
{
    return new CLocalResultActivityMaster(container);
}

class CGraphLoopResultWriteActivityMaster : public CLocalResultActivityMasterBase
{
public:
    CGraphLoopResultWriteActivityMaster(CMasterGraphElement *info) : CLocalResultActivityMasterBase(info)
    {
    }
    virtual void createResult()
    {
        IHThorGraphLoopResultWriteArg *helper = (IHThorGraphLoopResultWriteArg *)queryHelper();
        CGraphBase *graph = container.queryResultsGraph();
        graph->createGraphLoopResult(*this, inputRowIf, mergeResultTypes(thorgraphresult_distributed, thorgraphresult_grouped)); // NB graph owns result
    }
};

CActivityBase *createGraphLoopResultActivityMaster(CMasterGraphElement *container)
{
    return new CGraphLoopResultWriteActivityMaster(container);
}


class CDictionaryResultActivityMaster : public CLocalResultActivityMasterBase
{
public:
    CDictionaryResultActivityMaster(CMasterGraphElement *info) : CLocalResultActivityMasterBase(info)
    {
    }
    virtual void createResult() override
    {
        IHThorDictionaryResultWriteArg *helper = (IHThorDictionaryResultWriteArg *)queryHelper();
        CGraphBase *graph = container.queryResultsGraph();
        graph->createResult(*this, helper->querySequence(), this, mergeResultTypes(thorgraphresult_distributed, thorgraphresult_sparse)); // NB graph owns result
    }
};

CActivityBase *createDictionaryResultActivityMaster(CMasterGraphElement *container)
{
    return new CDictionaryResultActivityMaster(container);
}
