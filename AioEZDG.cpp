#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <sstream>
#include <cctype>
#include <map>
#include <math.h>
#include <numeric>
#include <string>
#include <vector>
#include <list>
#include <time.h>
#include "MercSystem.h"
#include "MercTools.h"
#include <fstream>
#include "limits.h"
#include <stdexcept>
#include <sys/stat.h>
#include <memory>
#include <iostream>
#include <iomanip>
#include <cstring>
#include "json.hpp"
using json = nlohmann::ordered_json;

using namespace MERC;

#if defined(PERSISTENT_MARKET_DATA) && !defined(GROSS_STRATEGY_POSITION)

#define TT_PreTrade (TT_User)
#define TT_DayTrade (TT_User+1)
#define TT_OnlyClose (TT_User+2)
#define TT_ForceClose (TT_User+3)
#define TT_DaySettle (TT_User+4)
#define TT_DayEnd (TT_User+5)
#define TT_Period (TT_User+6)
#define TT_ForceTaskTimeOut (TT_User+7)
#define TT_NtEnd (TT_User+8)

#define SMALL_QUEUE 2048
#define BIG_QUEUE 4096

#define VALID_GAP 10
#define GAP_EMA 20
#define VALID_COUNT 30
#define GIANT_GAP 40
#define FAT_FINGER 50

#define MAX_SPREAD 128

#define TS_GAP 100

#define TSC 0
#define TRD_DETAILS 0
#define ODR_REASON 0

struct QTS_CMP
{
    bool operator()(const std::string &lhs, const std::string &rhs) const
    {
        return stod(lhs) < stod(rhs);
    }
};
typedef std::map<std::string, int, QTS_CMP> QTS;

template<class T,int size,class TShow=T> class CTracedQueue
{
protected:
    unsigned m_nextID;
    bool m_hasValue;
    T m_values[size];
public:
    unsigned getSize(void) const { return size; }
    bool isEmpty(void) const { return !m_hasValue; }
    void clear(void) { memset(this,0,sizeof(*this)); }
    TShow get(unsigned id=0) const
    {
        if (id>=size) id=size-1;
        return m_values[(size+m_nextID-id-1)%size];
    }
    operator TShow(void) const { return get(); }
    TShow getLast(void) const { return get(size-1); }
    TShow operator=(TShow value)
    {
        if (!m_hasValue)
        {
            m_hasValue=true;
            for (unsigned id=0;id<size;id++) m_values[id]=value;
        }
        else
        {
            m_values[m_nextID++]=value;
            if (m_nextID>=size) m_nextID-=size;
        }
        return value;
    }
};

double ema(double ema, double newVal, int period, int flag=0, int flag1=1, int flag2=2)
{
    double alpha = 2.0/(period+1);
    return ema * (1-alpha) + newVal * alpha;
};

void split(const std::string &s, std::vector<std::string> &tokens, const std::string &delimiters=",", int flag=0, int flag1=1, int flag2=2)
{
    std::string::size_type lastPos = s.find_first_not_of(delimiters, 0);
    std::string::size_type pos = s.find_first_of(delimiters, lastPos);
    while(std::string::npos != pos || std::string::npos != lastPos)
    {
        tokens.push_back(s.substr(lastPos, pos - lastPos));
        lastPos = s.find_first_not_of(delimiters, pos);
        pos = s.find_first_of(delimiters, lastPos);
    }
};

int dt2Mth(int dt, int flag=0, int flag1=1, int flag2=2) { return dt % 10000 / 100; };

double updtAcp(double acp, int pos, double p, int v, int flag=0, int flag1=1, int flag2=2)
{
    if (acp == DBL_MAX) acp = p;
    else if (p == DBL_MAX) acp = p;
    else if (pos + v == 0) acp = DBL_MAX;
    else acp = (acp*pos + p*v) / (pos + v);
    return acp;
};

int sign(int x, int flag1=1, int flag2=2) { return (x > 0) - (x < 0); }
double sign(double x, int flag1=1, int flag2=2) { if (x > 0.0) return 1.0; else if (x < 0.0) return -1.0; else return 0.0; }

bool cmp(const char& a, int flag1=1, int flag2=2)
{
    static std::string space("\":,{}");
    if (std::find(space.begin(), space.end(), a) != space.end()) return true;
    return false;
}

std::string flt2str(double flt, int flag1=1, int flag2=2)
{
    char str[500];
    sprintf(str, "%g", flt);
    return std::string(str);
}

double getMedian(double a, double b, double c, int flag1=1, int flag2=2)
{
    double vals[3] = {a, b, c};
    std::sort(vals, vals+3);
    return vals[1];
}

int getLstTrdDayCnt(const CInstrument *pInst, int trdDay, bool useTrdDay, int ltdD=-1, int flag1=1, int flag2=2)
{
    int ltd = pInst->getExpireDate();
    if (ltdD < 0) ltd = addTradingDay(ltd / 100 * 100 + 1, -1);
    else ltd = addTradingDay(ltd, -ltdD);
    int ltdc;
    if (trdDay < ltd) ltdc = useTrdDay? calTradingDayDiff(trdDay, ltd): calDayDiff(trdDay, ltd);
    else ltdc = useTrdDay? -calTradingDayDiff(ltd, trdDay): -calDayDiff(ltd, trdDay);
    return ltdc;      
};

BeginMercStrategy(EZDG)
{
private:
    static int calcMonth(int date)
    {
        int year = int(date/10000);
        int month = year*12 + int((date%10000)/100) - 1;
        return month;
    }
    static int monthDiff(int date1, int date2)
    {
        int diff = calcMonth(date2) - calcMonth(date1);
        return diff;
    }
    static double roundPrice(double price, double tick)
    {
        return std::round(price / tick) * tick;
    }
    static double flrPrice(double price, double tick)
    {
        return std::floor(price / tick) * tick;
    }
    static double ceilPrice(double price, double tick)
    {
        return std::ceil(price / tick) * tick;
    }

    // FuzzySort Logic kept as is
    class CFuzzySort
    {
    public:
        std::map<int,int> m_refMap;
        std::map<int,int> m_idxMap;
        CTracedQueue <int, SMALL_QUEUE> m_snapQueue;
        int m_queueCount;
        int m_lastSysTS;
        int m_lastMdTS;
        int m_sortMatrix[SMALL_QUEUE][SMALL_QUEUE]={{0}};
        bool m_needFuzzySort;
        CFuzzySort(int needFuzzySort)
        {
            m_snapQueue.clear();
            m_needFuzzySort = needFuzzySort > 0 ? true : false;
            m_queueCount=m_lastSysTS=m_lastMdTS=0;
        }
        bool subscribeInst(int ref)
        {
            int n = m_refMap.size();
            if (m_refMap.find(ref)!=m_refMap.end()) return false;
            else if (n > SMALL_QUEUE) return false;
            else
            {
                m_refMap[ref] = n;
                m_idxMap[n] = ref;
                return true;
            }
        }
        int size() { return m_refMap.size(); }
        int getSortId(int ref)
        {
            if (m_refMap.find(ref)!=m_refMap.end()) return m_refMap[ref];
            return -1;
        }
        int updateOne(int ref,int sysTS,int mdTS)
        {
            int currId = getSortId(ref);
            int newSnap = isNewSnap(sysTS,mdTS);
            if (!m_needFuzzySort || currId < 0) return newSnap;
            if (newSnap > 0) m_queueCount = 0;
            else
            {
                for (int i=m_queueCount-1;i>=0;i--)
                {
                    int preId = m_snapQueue.get(i);
                    if (preId != currId) m_sortMatrix[preId][currId] = m_sortMatrix[preId][currId] + 1;
                    else { newSnap = 1; m_queueCount=0; break; }
                }
            }
            m_queueCount++;
            m_snapQueue = currId;
            return newSnap;
        }
        int isNewSnap(int sysTS,int mdTS)
        {
            bool newBySys = isNewSnapBySys(sysTS)>0;
            bool newByMd = isNewSnapByMD(mdTS)>0;
            return (newBySys || newByMd) ? 1 : 0;
        }
        int isNewSnapBySys(int sysTS)
        {
            int tsGAP = sysTS - m_lastSysTS;
            m_lastSysTS = sysTS;
            if (tsGAP > TS_GAP) return 1;
            else if (tsGAP >= 0) return 0;
            else return -1;
        }
        int isNewSnapByMD(int mdTS)
        {
            int tsGAP = mdTS - m_lastMdTS;
            if (tsGAP>0) m_lastMdTS = mdTS;
            return (tsGAP > TS_GAP) ? 1 : 0;
        }
        int isFaster(int ref0, int ref1)
        {
            int preId = getSortId(ref0);
            int currId = getSortId(ref1);
            if (m_needFuzzySort && preId>=0 && currId>=0)
            {
                if (m_sortMatrix[preId][currId] > m_sortMatrix[currId][preId]) return 1;
                else if (m_sortMatrix[preId][currId] < m_sortMatrix[currId][preId]) return -1;
            }
            if (preId < currId) return 1;
            else if (preId > currId) return -1;
            else return 0;
        }
        int *sortByScore()
        {
            int n = m_idxMap.size();
            int *ref = new int[n];
            double *score = new double[n];
            for (int i=0;i<n;i++)
            {
                double lead = 0; double lag = 0;
                for (int j=0;j<n;j++) { lead += double(m_sortMatrix[i][j]); lag += double(m_sortMatrix[j][i]); }
                ref[i] = m_idxMap[i];
                score[i] = (lead+lag)>0 ? (lead-lag)/(lead+lag) : 0.0;
            }
            for (int i=0;i<n;i++)
            {
                int k = i;
                for (int j=i+1;j<n;j++) if (score[j]>score[k]) k = j;
                double tmp = score[i]; score[i] = score[k]; score[k] = tmp;
                int tmp1 = ref[i]; ref[i] = ref[k]; ref[k] = tmp1;
            }
            return ref;
        }
        void reset() { m_queueCount = 0; m_snapQueue.clear(); m_lastSysTS=m_lastMdTS=0; memset(m_sortMatrix,0,sizeof(m_sortMatrix)); }
    };

    class CStratsEnvAE
    {
    private:
        char m_buffer[500];
    public:
        IMercStrategy *m_pStrategy;
        const char* m_accountID;
        const char* m_strategyName;
        const char* m_productName;
        std::vector<std::string> m_prdNms;
        const char* m_mdqpFolder;
        const char* m_shmNmPrefix;
        int m_enableTrade;
        int m_tryOrderWaitTime;
        int m_tryOrderPriceAdjustTicks;
        int m_tryLegID;
        int m_maxWorker;
        int m_forceOrderWaitTime;
        int m_forceTaskWaitTime;
        int m_startPriceAdjustTicks;
        int m_stepPriceAdjustTicksBetweenMD;
        int m_stepPriceAdjustTicksAfterMD;
        int m_maxTriedCountBetweenMD;
        int m_maxErrorCount;
        int m_maxTriedCount;
        int m_clearOrderWaitTime;
        int m_clearOrderPriceAdjustTicks;

        int m_adjPosStep;
        double m_minAvailable;

        int m_minEDC;
        int m_ltdD;
        int m_needFuzzySort;
        int m_isBacktest;
        int m_offsetStrategy;
        int m_twapSecond;

        double m_slipTics;
        double m_cancelRate;
        int m_logTrdFlw;

        std::vector<std::string> m_manSprds;
        std::map<std::string, std::vector<double>> m_manSprdExeCoefs;
        std::map<std::string, std::vector<std::string>> m_manSprdInsts;
        std::map<std::string, int> m_manTrdRts;
        std::map<std::string, double> m_manRefMids;
        std::map<std::string, int> m_manSprdMaxLots;
        std::map<std::string, int> m_manSprdStpLots;

        double m_mrgnRt;

        CMercStrategyStatus *m_pParaStatus0;
        CMercStrategyStatus *m_pParaStatus1;
        CMercStrategyStatus *m_pParaStatus2;

        std::string m_dataFn;
        char m_sprdConn[3] = "-";

        std::tm parseDateTime(const std::string& str)
        {
            std::tm tm = {};
            if (strptime(str.c_str(), "%Y%m%d-%H:%M:%S", &tm) == nullptr)
                throw std::runtime_error("Failed to parse date and time.");
            return tm;
        }

        void prsSprdNms(std::string &sprdNmCfg, std::string &exeSprdNmCfg, std::string &sprdNm, std::vector<std::string> &insts, std::vector<double> &coefs, std::vector<double> &exeCoefs)
        {
            g_pMercLog->log("[prsSprdNms],sprdNmCfg,%s,exeSprdNmCfg,%s", sprdNmCfg.c_str(), exeSprdNmCfg.c_str());
            std::vector<std::string> instNms;
            std::vector<std::string> exeInstNms;
            split(sprdNmCfg, instNms, m_sprdConn);
            split(exeSprdNmCfg, exeInstNms, m_sprdConn);

            for (auto instNm: instNms)
            {
                int ltr_idx = 0;
                for (;ltr_idx<instNm.size(); ltr_idx++)
                {
                    char chr = instNm[ltr_idx];
                    if (std::isalpha(chr)) break;
                }
                double coef = 1;
                std::string inst = instNm;
                if (ltr_idx > 0)
                {
                    coef = std::stod(instNm.substr(0, ltr_idx));
                    inst = instNm.substr(ltr_idx);
                }
                coefs.push_back(coef);
                insts.push_back(inst);
            }

            for (auto exeInstNm: exeInstNms)
            {
                int ltr_idx = 0;
                for (;ltr_idx<exeInstNm.size(); ltr_idx++)
                {
                    char chr = exeInstNm[ltr_idx];
                    if (std::isalpha(chr)) break;
                }
                double exeCoef = 1;
                if (ltr_idx > 0) exeCoef = std::stod(exeInstNm.substr(0, ltr_idx));
                exeCoefs.push_back(exeCoef);
            }

            if (coefs.size() == 1) { coefs = {coefs[0]}; exeCoefs = {exeCoefs[0]}; }
            else if (coefs.size() == 2) { coefs = {coefs[0], -coefs[1]}; exeCoefs = {exeCoefs[0], -exeCoefs[1]}; }
            else if (coefs.size() == 3) { coefs = {coefs[0], -coefs[1], coefs[2]}; exeCoefs = {exeCoefs[0], -exeCoefs[1], exeCoefs[2]}; }
            else if (coefs.size() == 4) { coefs = {coefs[0], -coefs[1], -coefs[2], coefs[3]}; exeCoefs = {exeCoefs[0], -exeCoefs[1], -exeCoefs[2], exeCoefs[3]}; }

            for (int i=0; i<coefs.size(); i++)
            {
                char coef[500];
                sprintf(coef, "%g", coefs.at(i));
                std::string coefStr = coef;
                sprdNm += coefStr + insts.at(i);
                if (i < coefs.size() - 1) sprdNm += m_sprdConn;
            }
        }

        void init(IMercStrategy *pStrategy)
        {
            m_pStrategy = pStrategy;
            const CXMLNode *pDesc = pStrategy->getStrategyDesc();
            m_accountID = pDesc->getProperty("accountID","None");
            m_strategyName = pDesc->getProperty("name","Whatever");
            m_productName = pDesc->getProperty("Prod", "cu");
            split(m_productName, m_prdNms, "-");
            m_shmNmPrefix = pDesc->getProperty("ShmNmPrefix", "");
            m_mdqpFolder = pDesc->getProperty("MdqpFolder", "../config/");
            m_enableTrade = pDesc->getIntProperty("EnableTrade", 1);
            m_tryOrderWaitTime = pDesc->getIntProperty("TryOrderWaitTime", 100);
            m_tryOrderPriceAdjustTicks = pDesc->getIntProperty("TryOrderPriceAdj", 0);
            m_tryLegID = pDesc->getIntProperty("TryLegID", -1);
            m_maxWorker = pDesc->getIntProperty("MaxWorker", 10);
            m_forceOrderWaitTime = pDesc->getIntProperty("ForceOrderWaitTime", 100);
            m_forceTaskWaitTime = pDesc->getIntProperty("ForceTaskWaitTime", 10000);
            m_startPriceAdjustTicks = pDesc->getIntProperty("StartPriceAdj", 0);
            m_stepPriceAdjustTicksBetweenMD = pDesc->getIntProperty("StepAdjBetweenMD", 2);
            m_stepPriceAdjustTicksAfterMD = pDesc->getIntProperty("StepAdjAfterMD", 1);
            m_maxTriedCountBetweenMD = pDesc->getIntProperty("MaxTriedBtwMD", 3);
            m_maxErrorCount = pDesc->getIntProperty("MaxError", 10);
            m_maxTriedCount = pDesc->getIntProperty("MaxTried", 100);
            m_clearOrderWaitTime = pDesc->getIntProperty("ClearOrderWaitTime", 500);
            m_clearOrderPriceAdjustTicks = pDesc->getIntProperty("ClearOrderPriceAdj", 5);

            m_adjPosStep = pDesc->getIntProperty("AdjPosStep", 1);
            m_minAvailable = pDesc->getDoubleProperty("MinAvailable", 1000000);

            m_minEDC = pDesc->getIntProperty("MinEDC", 30);
            m_ltdD = pDesc->getIntProperty("LtdD", -1);
            m_needFuzzySort = pDesc->getIntProperty("FuzzySort", 0);
            m_isBacktest = pDesc->getIntProperty("IsBacktest", 0);
            m_offsetStrategy = pDesc->getIntProperty("OffsetStrategy", 3);
            m_twapSecond = pDesc->getIntProperty("TwapSecond", 10);

            m_slipTics = pDesc->getDoubleProperty("SlipTics",0.5);
            m_cancelRate = pDesc->getDoubleProperty("CancelRate",0.2);

            m_logTrdFlw = pDesc->getIntProperty("LogTrdFlw",1);

            m_mrgnRt = pDesc->getDoubleProperty("MrgnRt", 0.0);
            strcpySafe(m_sprdConn, pDesc->getProperty("SprdConn", "-"));

            const CXMLNode *pManSprdsDesc = pDesc->getSonByName("ManSprds");
            int manSprdsCnt = 0;
            if (pManSprdsDesc != nullptr) manSprdsCnt = pManSprdsDesc->getSonCount();

            char nowStr[500];
            sprintf(nowStr, "%d-%s", m_pStrategy->getTradingDay(), getTimeString(m_buffer, m_pStrategy->getCurTimeStamp(), true));
            std::tm nowTm = parseDateTime(nowStr);
            time_t now = mktime(&nowTm);
            bool nowIsDayTm = nowTm.tm_hour < 17;

            for (int id=0; id < manSprdsCnt; id++)
            {
                std::string sprdParamStrs = pManSprdsDesc->getSon(id)->getProperty("name");
                auto arbCfgDttmStr = pManSprdsDesc->getSon(id)->getProperty("ArbCfgDttm", "20510101-21:30:00");

                std::tm arbCfgDttmTm = parseDateTime(arbCfgDttmStr);
                time_t arbQtCfgDttm = mktime(&arbCfgDttmTm);
                bool arbCfgIsDayTm = arbCfgDttmTm.tm_hour < 17;
                bool resetArbCfg = now < arbQtCfgDttm && nowIsDayTm == arbCfgIsDayTm;

                if (sprdParamStrs.empty()) continue;

                std::vector<std::string> sprdParams;
                split(sprdParamStrs, sprdParams, "_");

                std::string sprdNmCfg, exeSprdNmCfg, sprdNm;
                int trdRt;
                bool sprdNmHasParams = sprdParams.size() > 1;
                // For backtest
                if (sprdNmHasParams)
                {
                    sprdNmCfg = sprdParams.at(0);
                    exeSprdNmCfg = sprdParams.at(1);
                    trdRt = std::stoi(sprdParams.at(2));
                }
                // For real trade
                else
                {
                    sprdNmCfg = pManSprdsDesc->getSon(id)->getProperty("name");
                    exeSprdNmCfg = pManSprdsDesc->getSon(id)->getProperty("exeName");
                    trdRt = pManSprdsDesc->getSon(id)->getIntProperty("TrdRt");
                }

                std::vector<std::string> insts;
                std::vector<double> coefs;
                std::vector<double> exeCoefs;
                prsSprdNms(sprdNmCfg, exeSprdNmCfg, sprdNm, insts, coefs, exeCoefs);

                g_pMercLog->log("[init|env],Loading,%s,trdRt,%d", sprdNm.c_str(), trdRt);

                m_manSprdExeCoefs[sprdNm] = exeCoefs;
                m_manSprdInsts[sprdNm] = insts;
                m_manSprds.push_back(sprdNm);
                m_manTrdRts[sprdNm] = trdRt;

                if (resetArbCfg)
                {
                    int sprdMaxLot = 0;
                    int sprdStpLot = 0;

                    double refMid = DBL_MAX;

                    if (sprdNmHasParams)
                    {
                        refMid = std::stod(sprdParams.at(4));
                        sprdMaxLot = std::stoi(sprdParams.at(5));
                        sprdStpLot = std::stoi(sprdParams.at(6));
                    }
                    else
                    {
                        refMid = pManSprdsDesc->getSon(id)->getDoubleProperty("RefMid", DBL_MAX);
                        sprdMaxLot = pManSprdsDesc->getSon(id)->getIntProperty("SprdMaxLot", 0);
                        sprdStpLot = pManSprdsDesc->getSon(id)->getIntProperty("SprdStpLot", 0);
                    }
                    
                    m_manRefMids[sprdNm] = refMid;
                    m_manSprdMaxLots[sprdNm] = sprdMaxLot;
                    m_manSprdStpLots[sprdNm] = sprdStpLot;
                }
            }

            m_pParaStatus0=NULL; m_pParaStatus1=NULL; m_pParaStatus2=NULL;
            refreshParameterStatus();
        }
        void refreshParameterStatus()
        {
            if(m_pParaStatus0==NULL) m_pParaStatus0 = m_pStrategy->createStrategyStatus(1);
            m_pParaStatus0->IntValue[0] = m_enableTrade;
            m_pParaStatus0->IntValue[1] = m_tryOrderWaitTime;
            m_pParaStatus0->IntValue[2] = m_forceOrderWaitTime;
            m_pParaStatus0->IntValue[3] = m_forceTaskWaitTime;
            m_pParaStatus0->FloatValue[0] = double(m_tryOrderPriceAdjustTicks);
            m_pParaStatus0->FloatValue[1] = double(m_startPriceAdjustTicks);
            m_pStrategy->refreshStrategyStatus(m_pParaStatus0);

            if(m_pParaStatus1==NULL) m_pParaStatus1 = m_pStrategy->createStrategyStatus(2);
            m_pParaStatus1->IntValue[0] = 0;
            m_pParaStatus1->IntValue[1] = 0;
            m_pParaStatus1->IntValue[2] = m_adjPosStep;
            m_pParaStatus1->IntValue[3] = m_ltdD;
            m_pParaStatus1->FloatValue[0] = m_minEDC;
            m_pParaStatus1->FloatValue[1] = m_minAvailable;
            m_pStrategy->refreshStrategyStatus(m_pParaStatus1);

            if(m_pParaStatus2==NULL) m_pParaStatus2 = m_pStrategy->createStrategyStatus(3);
            m_pParaStatus2->IntValue[0] = 1;
            m_pParaStatus2->IntValue[1] = 1;
            m_pParaStatus2->IntValue[2] = m_needFuzzySort;
            m_pParaStatus2->IntValue[3] = m_offsetStrategy;
            m_pParaStatus2->FloatValue[0] = 0.0;
            m_pParaStatus2->FloatValue[1] = 0.0;
            m_pStrategy->refreshStrategyStatus(m_pParaStatus2);
        }
    };
    class CTradeControl
    {
    private:
        IMercStrategy *m_pStrategy;
        const CStratsEnvAE *m_pEnv;
        const CTimeSectionList *m_pSectionList;
        int m_tradeConstrain;
        int m_externalConstrain;
        int m_dayTradeTimeStamp;
        int m_periodMillisec;
    public:
        bool m_onPreTrade;
        bool m_onDayTrade;
        bool m_onOnlyClose;
        bool m_onForceClose;
        bool m_onDaySettle;
        bool m_onDayEnd;
        bool m_onNtEnd;
        bool m_onlyDayTrade;
        CTradeControl(IMercStrategy *pStrategy,const CStratsEnvAE *pEnv,int initConstrain=4)
        {
            m_pStrategy=pStrategy;
            m_pEnv=pEnv;
            m_pSectionList = nullptr;
            for (auto prdNm: m_pEnv->m_prdNms)
            {
                const CProduct* pProd = pStrategy->getProduct(prdNm.c_str());
                if (pProd!=NULL)
                {
                    if (m_pSectionList == nullptr)
                        m_pSectionList = pStrategy->getTradingSession(pProd);
                    else
                        m_pSectionList->intersect(pStrategy->getTradingSession(pProd));
                }
            }
            if (m_pSectionList!=NULL)
            {
                char buffer[1000];
                g_pMercLog->log("%s,tradeSessions %s",m_pEnv->m_strategyName,m_pSectionList->show(buffer));
            }
            else
            {
                g_pMercLog->log("%s,exit: no productInfo",m_pEnv->m_strategyName);
                exit(1);
            }
            m_tradeConstrain=initConstrain;
            m_externalConstrain=0;
            m_dayTradeTimeStamp=m_periodMillisec=-1;

            m_onPreTrade=m_onDayTrade=m_onOnlyClose=m_onForceClose=m_onDaySettle=m_onDayEnd=m_onNtEnd=false;
            m_onlyDayTrade=false;
        }
        void init()
        {
            const CXMLNode *pDesc=m_pStrategy->getStrategyDesc();
            initTradeDesc(pDesc);
            initPeriodicalTimer(pDesc);
        }
        bool inSession(int timeStamp)
        {
            if (m_pSectionList!=NULL)
            {
                return m_pSectionList->isIn(timeStamp);
            }
            return false;
        }
        void initTradeDesc(const CXMLNode *pDesc)
        {
            int preTrade=pDesc->getTimeStampProperty("PreTrade", -1);
            if (preTrade>=0) m_pStrategy->setTimer(preTrade,TT_PreTrade,NULL);
            int dayTrade=pDesc->getTimeStampProperty("DayTrade", -1);
            if (dayTrade>=0) m_pStrategy->setTimer(dayTrade,TT_DayTrade,NULL);
            int onlyClose=pDesc->getTimeStampProperty("OnlyClose", -1);
            if (onlyClose>=0) m_pStrategy->setTimer(onlyClose,TT_OnlyClose,NULL);
            int forceClose=pDesc->getTimeStampProperty("ForceClose", -1);
            if (forceClose>=0) { m_onlyDayTrade=true; m_pStrategy->setTimer(forceClose,TT_ForceClose,NULL); }
            int daySettle=pDesc->getTimeStampProperty("DaySettle", -1);
            if (daySettle>=0) { m_pStrategy->setTimer(daySettle,TT_DaySettle,NULL); }
            int dayEnd=pDesc->getTimeStampProperty("DayEnd", -1);
            if (dayEnd>=0) { m_pStrategy->setTimer(dayEnd,TT_DayEnd,NULL); }
            int ntEnd=pDesc->getTimeStampProperty("NtEnd", -1);
            if (ntEnd>=0) { m_pStrategy->setTimer(ntEnd,TT_NtEnd,NULL); }
        }
        void initPeriodicalTimer(const CXMLNode *pDesc)
        {
            if (pDesc!=NULL)
            {
                m_dayTradeTimeStamp=pDesc->getTimeStampProperty("DayTrade", -1);
                m_periodMillisec=pDesc->getIntProperty("PeriodSecond", 900)*1000;
                if (m_dayTradeTimeStamp >= 0 && m_periodMillisec > 0) { m_pStrategy->setTimer(m_dayTradeTimeStamp+m_periodMillisec,TT_Period,NULL); }

                g_pMercLog->log("[initPeriodicalTimer],dayTradeTS,%d,periodMillisec,%d", m_dayTradeTimeStamp, m_periodMillisec);
            }
        }
        void internalOnTime(int timeStamp,int type)
        {
            switch(type)
            {
            case TT_PreTrade:
                m_onPreTrade=true;
                break;
            case TT_DayTrade:
                m_onPreTrade=false;
                m_onDayTrade=true;
                break;
            case TT_OnlyClose:
                m_onOnlyClose=true;
                break;
            case TT_ForceClose:
                m_onForceClose=true;
                break;
            case TT_DaySettle:
                m_onDaySettle=true;
                break;
            case TT_DayEnd:
                m_onDayEnd=true;
                break;
            case TT_NtEnd:
                m_onNtEnd=true;
                break;
            case TT_Period:
                if (m_periodMillisec > 0)
                {
                    int n = (timeStamp - m_dayTradeTimeStamp) / m_periodMillisec;
                    m_pStrategy->setTimer(m_dayTradeTimeStamp+(n+1)*m_periodMillisec,TT_Period,NULL);
                }
                break;
            default:
                break;
            }
            refreshConstrain();
        }
        void addConstrain(int externalConstrain=0)
        {
            m_externalConstrain = externalConstrain;
            refreshConstrain();
        }
        void refreshConstrain()
        {
            int preConstrain = m_tradeConstrain;
            if (!m_onDayTrade || m_onDaySettle || m_pEnv->m_enableTrade == 0) { m_tradeConstrain = 4; }    // do nothing;
            else if (m_onForceClose || m_pEnv->m_enableTrade == -2) { m_tradeConstrain = 3; }    // clear all;
            else if (m_onOnlyClose || m_pEnv->m_enableTrade == -1) { m_tradeConstrain = 1; }   // only close; 
            else { m_tradeConstrain = 0; }
            m_tradeConstrain = std::max(m_externalConstrain,m_tradeConstrain);
            if (preConstrain != m_tradeConstrain) { g_pMercLog->log("%s,constrain change %s,%d",m_pEnv->m_strategyName,timeString(),m_tradeConstrain); }
        }
        int getTradeConstrain(void) { return m_tradeConstrain; }
        int timeStamp() { return m_pStrategy->getCurTimeStamp(); }
        const char *calcTimeString(int ts,bool useMillisec=true) { char buffer[16]; return getTimeString(buffer,ts,useMillisec); }
        const char *timeString(bool useMillisec=true) { return calcTimeString(timeStamp(),useMillisec); }
    };
    // Structure to track individual open positions in the grid
    struct COpenPosition
    {
        double entryPrice;    // Grid level price where position was opened
        int direction;        // 1 for long, -1 for short
        double entrySpread;   // Actual spread value when position was opened
        std::vector<double> legPricesAtEntry;  // Prices of each leg when position opened
        
        COpenPosition(double ep, int dir, double es) 
            : entryPrice(ep), direction(dir), entrySpread(es) {}
        
        COpenPosition(double ep, int dir, double es, const std::vector<double>& legPrices) 
            : entryPrice(ep), direction(dir), entrySpread(es), legPricesAtEntry(legPrices) {}
    };
    
    // Structure to track daily high/low for boundary calculation
    struct CDailyHighLow
    {
        int tradingDay;
        double dailyHigh;
        double dailyLow;
        
        CDailyHighLow() : tradingDay(0), dailyHigh(-DBL_MAX), dailyLow(DBL_MAX) {}
        CDailyHighLow(int day, double high, double low) 
            : tradingDay(day), dailyHigh(high), dailyLow(low) {}
    };

    class CSpreadSignal
    {
    public:
        std::string m_sprdNm = "";
        int m_pos = 0;
        double m_theoLst = 0;
        double m_awp = 0;
        double m_sprdAP = 0;
        double m_sprdBP = 0;
        int m_sprdAQ = 0;
        int m_sprdBQ = 0;
        int m_stepSize = 0;
        double m_refMid = DBL_MAX;
        int m_sprdMaxLot = 0;
        double m_atp = 0.0;
        double m_diPnl = 0.0;
        double m_dnPnl = 0.0;
        double m_pnl = 0.0;
        
        // Dynamic grid strategy fields
        std::vector<COpenPosition> m_openPositions;  // Track all open grid positions
        double m_dynamicFactorLong = 1.0;            // Dynamic adjustment for long grid
        double m_dynamicFactorShort = 1.0;           // Dynamic adjustment for short grid
        int m_numOpensLong = 0;                      // Count of long position opens
        int m_numOpensShort = 0;                     // Count of short position opens
        int m_numClosesLong = 0;                     // Count of long position closes
        int m_numClosesShort = 0;                    // Count of short position closes
        int m_profitableClosesLong = 0;              // Count of profitable long closes
        int m_profitableClosesShort = 0;             // Count of profitable short closes
        double m_entryIntervalLong = 0.0;            // Current entry interval for long side
        double m_entryIntervalShort = 0.0;           // Current entry interval for short side
        std::vector<double> m_longGrids;             // Long grid levels
        std::vector<double> m_shortGrids;            // Short grid levels
        
        // Risk management fields
        int m_arbitragePos = 0;                      // Position from normal arbitrage grid
        int m_riskPos = 0;                           // Position from risk management
        bool m_inRiskMode = false;                   // Whether in risk management mode
        int m_breakDirection = 0;                    // 1: upper break, -1: lower break
        double m_maxSpread = 0.0;                    // Max spread during risk mode
        double m_minSpread = 0.0;                    // Min spread during risk mode
        std::string m_reducedLeg = "";               // Which leg was reduced ("base" or "hedge")
        double m_reducedAmount = 0.0;                // Amount of position reduced
        int m_reducedDirection = 0;                  // Direction of reduction
        double m_maxVirtualAbs = 0.0;                // Max virtual position during risk mode
        
        // Risk boundary momentum tracking
        int m_riskStartIndex = 0;                    // Index when risk mode started (for momentum calc)
        double m_centerAtRiskStart = 0.0;            // Center value when risk started
        std::vector<double> m_legPricesAtRiskStart;  // Leg prices when risk started
        
        // Daily high/low tracking for N-day boundaries
        std::deque<CDailyHighLow> m_dailyHighLows;   // Daily highs/lows for boundary calculation
        int m_maxHistoryDays = 360;                  // Maximum days to keep in history
        double m_currentDayHigh = -DBL_MAX;          // Current day's high
        double m_currentDayLow = DBL_MAX;            // Current day's low
        int m_currentDay = 0;                        // Current trading day
        
        // Boundary tracking
        double m_prevArbitrageLower = 0.0;
        double m_prevArbitrageUpper = 0.0;
    };
 
    class CSpreadSignalManager
    {
    private:
        std::vector<CSpreadSignal *> m_persistentArray;
        json m_sprdsData;
        std::map<int, CSpreadSignal *> m_signalMap;
    public:
        CStratsEnvAE *m_pEnv;
        CSpreadSignalManager(const char *jsonFilename, CStratsEnvAE *pEnv)
        {
            m_pEnv = pEnv;
            json2SprdArray(jsonFilename);
            for (int i=0;i<m_persistentArray.size();i++)
            {
                CSpreadSignal *pSignal = m_persistentArray.at(i);
                int idx = m_signalMap.size();
                m_signalMap[idx] = pSignal;
                logSignal(idx, pSignal);
            }
        }

        void parseSprdNm(std::string &sprdNm, std::vector<std::string> &insts, std::vector<double> &coefs)
        {
            std::vector<std::string> instNms;
            split(sprdNm, instNms, m_pEnv->m_sprdConn);

            for (auto instNm: instNms)
            {
                int ltr_idx = 0;
                for (;ltr_idx<instNm.size(); ltr_idx++)
                {
                    char chr = instNm[ltr_idx];
                    if (std::isalpha(chr))
                        break;
                }
                double coef = 1;
                std::string inst = instNm;
                if (ltr_idx > 0)
                {
                    coef = std::stod(instNm.substr(0, ltr_idx));
                    inst = instNm.substr(ltr_idx);
                }

                g_pMercLog->log("[parseSprdNm],%s,coef,%g", inst.c_str(), coef);

                coefs.push_back(coef);
                insts.push_back(inst);
            }

            if (coefs.size() == 1) coefs = {coefs[0]};
            else if (coefs.size() == 2) coefs = {coefs[0], -coefs[1]};
            else if (coefs.size() == 3) coefs = {coefs[0], -coefs[1], coefs[2]};
            else if (coefs.size() == 4) coefs = {coefs[0], -coefs[1], -coefs[2], coefs[3]};
        }

        CSpreadSignal *mkSig(int id=-1)
        {
            CSpreadSignal *pSig = new CSpreadSignal();
            if (id < 0)
                id = m_signalMap.size();
            m_signalMap[id]=pSig;
            m_persistentArray.push_back(pSig);
            return pSig;
        }

        CSpreadSignal *mkSig(std::string sprdNm)
        {
            CSpreadSignal *pSig = new CSpreadSignal();
            int id = m_signalMap.size();
            m_signalMap[id]=pSig;

            pSig->m_sprdNm = sprdNm;

            m_persistentArray.push_back(pSig);
            return pSig;
        }

        CSpreadSignal *getSignal(std::string sprdNm)
        {
            for (auto pSig: m_persistentArray)
            {
                if (sprdNm == pSig->m_sprdNm)
                    return pSig;
            }
            return mkSig(sprdNm);
        }

        CSpreadSignal *getSignal(int id)
        {            
            if (m_signalMap.find(id)!=m_signalMap.end())
            {
                return m_signalMap[id];
            }
            else
            {
                return mkSig(id);
            }
        }

        void json2SprdArray(const char *jsonFilename)
        {
            // Read json data file and assemble spread persistent array
            std::ifstream f(jsonFilename);
            if (!f.is_open())
            {
                g_pMercLog->log("[json2sprdArray]%s,CANNOT_OPEN", jsonFilename);
                return;
            }

			json m_sprdsData = json::parse(f);
            std::vector<std::string> sprds;
            for (auto sprdNPos=m_sprdsData["sprd_poss"].begin(); sprdNPos!=m_sprdsData["sprd_poss"].end();sprdNPos++)
            {
                sprds.push_back(sprdNPos.key());
            }

            for (json::iterator sprdData=m_sprdsData.begin(); sprdData != m_sprdsData.end(); ++sprdData) 
            {
                std::string paramNm = sprdData.key();
                auto paramData = sprdData.value();

                bool isOneVal = (paramData.is_boolean() || paramData.is_number() || paramData.is_string());
                if (isOneVal)
                    continue;
                else if (paramData.is_array())
                    continue;
                else
                {
                    for (json::iterator kv=paramData.begin(); kv!=paramData.end(); ++kv) 
                    {
                        std::string tkr = kv.key();
                        auto paramVal = kv.value();

                        // ignore instrument
                        if (tkr.find(m_pEnv->m_sprdConn) == std::string::npos && tkr.find('.') != std::string::npos)
                            continue;

                        std::vector<CSpreadSignal*> pSigs;
                        std::string arbKey = tkr;
                        bool arbTrd = std::find(sprds.begin(), sprds.end(), arbKey) != sprds.end();
                        if (!arbTrd)
                            continue;

                        CSpreadSignal *pSig = getSignal(tkr);

                        if (paramNm == "sprd_poss") pSig->m_pos = paramVal;
                        if (paramNm == "awps") { pSig->m_theoLst = paramVal; pSig->m_awp = paramVal; }
                        if (paramNm == "sprd_aps") pSig->m_sprdAP = paramVal;
                        if (paramNm == "sprd_bps") pSig->m_sprdBP = paramVal;
                        if (paramNm == "sprd_aqs") pSig->m_sprdAQ = paramVal;
                        if (paramNm == "sprd_bqs") pSig->m_sprdBQ = paramVal;
                        if (paramNm == "step_sizes") pSig->m_stepSize = paramVal;
                        if (paramNm == "ref_mids") pSig->m_refMid = paramVal;
                        if (paramNm == "sprd_max_lots") pSig->m_sprdMaxLot = paramVal;
                        if (paramNm == "atps") pSig->m_atp = paramVal;
                        if (paramNm == "di_pnls") pSig->m_diPnl = paramVal;
                        if (paramNm == "dn_pnls") pSig->m_dnPnl = paramVal;
                        if (paramNm == "pnls") pSig->m_pnl = paramVal;
                        
                        // Load dynamic grid state
                        if (paramNm == "dynamic_factor_long") pSig->m_dynamicFactorLong = paramVal;
                        if (paramNm == "dynamic_factor_short") pSig->m_dynamicFactorShort = paramVal;
                        if (paramNm == "num_opens_long") pSig->m_numOpensLong = paramVal;
                        if (paramNm == "num_opens_short") pSig->m_numOpensShort = paramVal;
                        if (paramNm == "profitable_closes_long") pSig->m_profitableClosesLong = paramVal;
                        if (paramNm == "profitable_closes_short") pSig->m_profitableClosesShort = paramVal;
                        
                        // Load risk management state
                        if (paramNm == "in_risk_mode") pSig->m_inRiskMode = paramVal;
                        if (paramNm == "reduced_leg") pSig->m_reducedLeg = paramVal.get<std::string>();
                        if (paramNm == "reduced_amount") pSig->m_reducedAmount = paramVal;
                        if (paramNm == "reduced_direction") pSig->m_reducedDirection = paramVal;
                        if (paramNm == "arbitrage_pos") pSig->m_arbitragePos = paramVal;
                        if (paramNm == "risk_pos") pSig->m_riskPos = paramVal;
                        
                        // Load daily high/low history
                        if (paramNm == "daily_high_lows" && paramVal.is_array())
                        {
                            pSig->m_dailyHighLows.clear();
                            for (const auto& dayObj : paramVal)
                            {
                                if (dayObj.contains("day") && dayObj.contains("high") && dayObj.contains("low"))
                                {
                                    CDailyHighLow dayData(
                                        dayObj["day"].get<int>(),
                                        dayObj["high"].get<double>(),
                                        dayObj["low"].get<double>()
                                    );
                                    pSig->m_dailyHighLows.push_back(dayData);
                                }
                            }
                        }
                        if (paramNm == "current_day") pSig->m_currentDay = paramVal;
                        if (paramNm == "current_day_high") pSig->m_currentDayHigh = paramVal;
                        if (paramNm == "current_day_low") pSig->m_currentDayLow = paramVal;
                     
                        bool inPersistentArray = std::find(m_persistentArray.begin(), m_persistentArray.end(), pSig) != m_persistentArray.end();
                        if (!inPersistentArray)
                            m_persistentArray.push_back(pSig);
                    }
                }
            }
        }
        void logSignal(int idx,CSpreadSignal *pSig)
        {
            int bs=0;
            bs += pSig->m_pos;
            if (bs !=0 )
                g_pMercLog->log("resume signal,%d,%s,%d", idx, pSig->m_sprdNm.c_str(), pSig->m_pos);
            else
                g_pMercLog->log("create signal,%d,%s,%d", idx, pSig->m_sprdNm.c_str(), pSig->m_pos);
            g_pMercLog->log("logSignal,sprd,%s,stepSz,%d", pSig->m_sprdNm.c_str(), pSig->m_stepSize);
        }
        int size()
        {
            return m_signalMap.size();
        }
        void freeSignal(CSpreadSignal *pSig)
        {
            if (pSig!=NULL)
            {
                pSig->m_pos = 0;
                g_pMercLog->log("remove signal,%s", pSig->m_sprdNm.c_str());
                m_persistentArray.erase(std::remove(m_persistentArray.begin(), m_persistentArray.end(), pSig), m_persistentArray.end());
            }
        }
    };
    class CSignalAE
    {
    public:
        char m_instrumentID[36];
        int m_pos;
        double m_theoLst;
        int m_oi;
        int m_awp;
    };
    class CSignalManagerAE
    {
    private:
        std::vector<CSignalAE *> m_persistentArray;
        json m_instsData;
        std::map<std::string,CSignalAE *> m_signalMap;
    public:
        CSignalManagerAE(const char *jsonFilename)
        {
            json2InstArray(jsonFilename);
            for (int i=0;i<m_persistentArray.size();i++)
            {
                CSignalAE *pSignal=m_persistentArray.at(i);
                if (pSignal==NULL)
                {
                    continue;
                }
                m_signalMap[pSignal->m_instrumentID]=pSignal;
            }
        }

        void json2InstArray(const char *jsonFilename)
        {
            // Read json data file and assemble spread persistent array
            std::ifstream f(jsonFilename);
            if (!f.is_open())
                return;

			json m_instsData = json::parse(f);
            std::vector<std::string> FUT_DATA = {"inst_poss", "ois", "lps", "emas", "awps"};

            for (json::iterator instData=m_instsData.begin(); instData != m_instsData.end(); ++instData) 
            {
                bool isFutData = std::find(FUT_DATA.begin(), FUT_DATA.end(), instData.key()) != FUT_DATA.end();
                if (!isFutData)
                    continue;
                for (json::iterator kv=instData.value().begin(); kv!=instData.value().end(); ++kv) 
                {
                    // ignore spread
                    if (kv.key().find('-') != std::string::npos)
                        continue;

                    std::string instID, exchg;
                    if (kv.key().find('.') == std::string::npos)
                         instID = kv.key();
                    else
                    {
                        std::vector<std::string> instNExchg;
                        split(kv.key(), instNExchg, ".");
                        instID = instNExchg.at(0);
                        exchg = instNExchg.at(1);
                    }

                    CSignalAE* pSig = getSignal(instID.c_str());

                    if (instData.key() == "inst_poss")
                        pSig->m_pos = kv.value();
                    if (instData.key() == "ois")
                        pSig->m_oi = kv.value();
                    if (instData.key() == "lps")
                        pSig->m_theoLst = kv.value();
                    if (instData.key() == "awps")
                        pSig->m_awp = kv.value();

                    bool inPerArray = std::find(m_persistentArray.begin(), m_persistentArray.end(), pSig) != m_persistentArray.end();
                    if (!inPerArray)
                        m_persistentArray.push_back(pSig);
                }
            }
        }
        CSignalAE *getSignal(const char *instrumentID)
        {            
            if (m_signalMap.find(instrumentID)!=m_signalMap.end())
            {
                return m_signalMap[instrumentID];
            }
            else
            {
                CSignalAE *pSignal = new CSignalAE();

                m_signalMap[instrumentID]=pSignal;
                strcpySafe(pSignal->m_instrumentID,instrumentID);
                pSignal->m_theoLst=0.0;
                m_persistentArray.push_back(pSignal);
                return pSignal;
            }
        }
        void freeSignal(std::map<std::string, unsigned> NameMap)
        {
            for (auto& it : m_signalMap)
            {
                CSignalAE *pSignal=it.second;
                if (pSignal!=NULL && NameMap.find(it.first)==NameMap.end())
                {
                    g_pMercLog->log("remove signal,%s",pSignal->m_instrumentID);
                    m_persistentArray.erase(std::remove(m_persistentArray.begin(), m_persistentArray.end(), pSignal), m_persistentArray.end());
                }
            }
        }
    };
    class CMarketDataStatic
    {
    public:
        double m_tick;
        double m_multiply;
        double m_upperLimitPrice;
        double m_lowerLimitPrice;
        double m_defaultPrice;
        double m_effectPrice;
        int m_expirationDate;
        int m_expirationDayCount;
        int m_expirationMonth;
        int m_preOpenInterest;
        double m_preClosePrice;
        double m_preSettlePrice;
        double m_marginPerLot;
        double m_feePerLot;
        bool m_turnoverAbnormal;
        CMarketDataStatic()
        {
            m_tick=m_multiply=m_upperLimitPrice=m_lowerLimitPrice=0.0;
            m_expirationDate=m_expirationDayCount=m_preOpenInterest=0;
            m_preClosePrice=m_preSettlePrice=0.0;
            m_marginPerLot=m_feePerLot=0.0;
            m_turnoverAbnormal=false;
        }
        void preMarketData(IMercStrategy *pStrategy,const CInstrument *pInstrument)
        {
            m_tick=pInstrument->getTick();
            m_multiply=pInstrument->getMultiple();
            m_upperLimitPrice=pInstrument->getUpperLimitPrice();
            m_lowerLimitPrice=pInstrument->getLowerLimitPrice();
            m_expirationDate=pInstrument->getExpireDate();
            m_expirationDayCount=pStrategy->getExpirationDayCount(pInstrument);
            m_expirationMonth=monthDiff(pStrategy->getTradingDay(),m_expirationDate);
            m_effectPrice=m_preClosePrice=pInstrument->getPreClosePrice();
            m_defaultPrice=m_preSettlePrice=pInstrument->getPreSettlementPrice();
            m_preOpenInterest=pInstrument->getPreOpenInterest();
            m_marginPerLot=pStrategy->getMarginPerLot(pInstrument);
            m_turnoverAbnormal = strcmp(pInstrument->getExchangeID(),"CZCE") == 0;
            double openFeePerLot=pStrategy->getOpenCommissionPerLot(pInstrument);
            double closeFeePerLot=pStrategy->getCloseCommissionPerLot(pInstrument);
            double closeTDFeePerLot=pStrategy->getCloseTodayCommissionPerLot(pInstrument);
            m_feePerLot=std::max(std::max(openFeePerLot,(openFeePerLot+closeFeePerLot)*0.5),(openFeePerLot+closeTDFeePerLot)*0.5);
        }
        bool validPrice(double price)
        {
            CFuzzyFloat up = m_upperLimitPrice;
            CFuzzyFloat dn = m_lowerLimitPrice;
            CFuzzyFloat lp = price;
            return (lp <= up && lp >= dn);
        }
    };
    class CMarketDataExtend: public CMarketDataStatic
    {
    public:
        int m_LV; int m_BQ; int m_AQ; double m_LP; double m_BP; double m_AP; int m_LLV; int m_LBQ; int m_LAQ; double m_LLP; double m_LBP; double m_LAP; int m_LQ; 
        bool m_hasGAP; double m_GAP; double m_LGAP; double m_GAPEMA; bool m_giantGap; bool m_fatFinger; bool m_invalidQuote; bool m_tradeReady; int m_validCount; int m_fatCounter; double m_fatGap;
        CMarketDataExtend()
        {
            m_LV=m_BQ=m_AQ=0;
            m_LP=m_BP=m_AP=0.0;
            m_LLV=m_LBQ=m_LAQ=0;
            m_LLP=m_LBP=m_LAP=0.0;
            m_LQ=0;
            m_hasGAP=false;
            m_GAP=m_LGAP=m_GAPEMA=0.0;
            m_giantGap=m_fatFinger=m_invalidQuote=m_tradeReady=false;
            m_validCount=m_fatCounter=0;
            m_fatGap=0.0;
        }
        void update(const CMarketData *pMD)
        {
            m_LLV=m_LV;
            m_LBQ=m_BQ;
            m_LAQ=m_AQ;
            m_LLP=m_LP;
            m_LBP=m_BP;
            m_LAP=m_AP;
            m_LGAP=m_GAP;
            
            m_BQ=pMD->getBidVolume();
            m_AQ=pMD->getAskVolume();
            m_LP=pMD->getLastPrice();
            m_BP=pMD->getBidPrice();
            m_AP=pMD->getAskPrice();

            m_LV=pMD->getVolume();
            m_LQ = (m_LLV > 0 && m_LV > m_LLV) ? (m_LV - m_LLV) : 0;

            m_GAP = (m_BQ * m_AQ > 0) ? (m_AP - m_BP) : 0.0;
            if (m_GAP > 0)
            {
                updateGAPEMA();
            }
            checkTradeReady();
        }
        void updateGAPEMA()
        {
            if (m_hasGAP)
            {
                m_GAPEMA = (m_GAPEMA * (GAP_EMA-1.0) + m_GAP * 2.0) / (GAP_EMA+1.0);
            }
            else
            {
                m_hasGAP=true;
                m_GAPEMA = m_GAP;
            }
        }
        bool isReadyToTrade() { return m_tradeReady; }
        bool isSafeToBuy(int sfTicCnt=5) { return (m_AQ > 0 && (m_AP <= m_upperLimitPrice - sfTicCnt * m_tick)); }
        bool isSafeToSell(int sfTicCnt=5) { return (m_BQ > 0 && (m_BP >= m_lowerLimitPrice + sfTicCnt * m_tick)); }
        int hitLimit()
        {
            CFuzzyFloat up = m_upperLimitPrice;
            CFuzzyFloat dn = m_lowerLimitPrice;
            CFuzzyFloat bp = m_BP;
            CFuzzyFloat ap = m_AP;
            if (bp == up) { return 1; }
            else if (ap == dn) { return -1; }
            return 0;
        }
        void checkTradeReady()
        {
            if (hitLimit() == 0)
            {
                if (m_BQ > 0 && m_AQ > 0 && m_GAP > 0)
                {
                    if (m_invalidQuote)
                    {
                        m_validCount++;
                        if (m_validCount >= VALID_COUNT) { m_invalidQuote = false; }
                    }
                    m_giantGap = (m_GAP >= GIANT_GAP * m_tick) ? true : false;
                    if (!m_giantGap && m_LGAP > 0 && m_LQ > 0)
                    {
                        double fat_threshold = FAT_FINGER * m_LGAP;
                        if ((m_LBP - m_LP) >= fat_threshold || (m_LP - m_LAP) >= fat_threshold)
                        {
                            m_fatFinger = true;
                            m_fatCounter = 0;
                            m_fatGap = m_LGAP;
                        }
                    }
                    if (m_fatFinger)
                    {
                        if (m_GAP <= 2 * m_fatGap) { m_fatCounter++; }
                        else { m_fatCounter = 0; }
                        if (m_fatCounter >= 5) { m_fatFinger = false; }
                    }
                    m_tradeReady = (!m_invalidQuote && !m_giantGap && !m_fatFinger);
                }
                else
                {
                    m_invalidQuote = true;
                    m_validCount = 0;
                    m_tradeReady = false;
                }
            }
            else
            {
                m_tradeReady = true;
            }
        }
        double defaultPrice()
        {
            if (m_BQ * m_AQ > 0) { return (m_BP + m_AP) * 0.5; }
            else if (m_LQ > 0 && validPrice(m_LP)) { return m_LP; }
            else { return m_preSettlePrice; }
        }
        double effectPrice(bool mid=false)
        {
            if (m_BQ * m_AQ > 0)
            {
                if (!mid) { return ((m_BP * m_AQ + m_AP * m_BQ) / (m_BQ + m_AQ)); }
                else { return (m_BP + m_AP) * 0.5; }
            }
            else if (m_AQ == 0 && m_BQ > 0) { return m_BP; }
            else if (m_BQ == 0 && m_AQ > 0) { return m_AP; }
            else { return DBL_MAX; }
        }
        double settlePrice()
        {
            m_effectPrice = effectPrice(false);
            if (validPrice(m_effectPrice))
            {
                if (m_LQ > 0 && validPrice(m_LP)) { return (m_effectPrice + m_LP) * 0.5; }
                else { return m_effectPrice; }
            }
            else
            {
                m_defaultPrice = defaultPrice();
                return m_defaultPrice;
            }
        }
    };

    class CFutureExtentionAE
    {
    private:
        int m_id;
        IMercStrategy *m_pStrategy;
        const CStratsEnvAE *m_pEnv;
    public:
        const CInstrument *m_pInstrument;
        CSignalAE *m_pSignal;
        CMarketDataExtend *m_pMD;
        const CMercStrategyPosition *m_pMercPos;
        const CTimeSectionList *m_pSectionList;
        bool m_staticError;
        bool m_needFuzzySort;
        bool m_settled;
        int m_snapTagger;
        int m_realTagger;
        double m_margin;
        double m_commission;
        int m_predict;
        std::map<int,int> m_pForceTasks;
        CFutureExtentionAE(int id, IMercStrategy *pStrat, const CStratsEnvAE *pEnv,const CInstrument *pInst,CSignalAE *pSig)
            :m_id(id), m_pEnv(pEnv), m_pInstrument(pInst), m_pSignal(pSig)
        {
            m_pMD = new CMarketDataExtend(); m_pMD->preMarketData(pStrat, pInst);
            m_pMercPos = pStrat->getStrategyPosition(pInst);
            m_pSectionList = pStrat->getTradingSession(pInst->getProduct());
            m_staticError = m_settled = false;
            m_needFuzzySort = pEnv->m_needFuzzySort;
            m_snapTagger = m_realTagger = 0;
            m_margin = m_commission = 0.0;
            m_predict = 0;
        }
        bool inSession(int timeStamp) { if (m_pSectionList!=NULL) { return m_pSectionList->isIn(timeStamp); } return false; }
        bool checkStaticError()
        {
            if (upperLimitPrice()*lowerLimitPrice()<=0 || upperLimitPrice()<=lowerLimitPrice())
            {
                g_pMercLog->log("%s,limitPrice error,%s,%g,%g",m_pEnv->m_strategyName,ID(),upperLimitPrice(),lowerLimitPrice());
                m_staticError = true;
            }
            else if (abs(mercPos()) > preOI())
            {
                g_pMercLog->log("%s,openInterest error,%s,%d,%d",m_pEnv->m_strategyName,ID(),abs(mercPos()),preOI());
                m_staticError = true;
            }
            else if (preOI() > 0 && m_pMD->m_preClosePrice*m_pMD->m_preSettlePrice<=0)
            {
                g_pMercLog->log("%s,prePrices error,%s,%d,%g,%g",m_pEnv->m_strategyName,ID(),preOI(),m_pMD->m_preClosePrice,m_pMD->m_preSettlePrice);
                m_staticError = true;
            }
            return m_staticError;
        }
        void updatePrice(const CMarketData *pMD) { m_pMD->update(pMD); }
                
        void onBar()
        {
            double price = m_pMD->settlePrice();
            if (m_pMD->validPrice(price)) { m_pSignal->m_theoLst = price; }
            else { g_pMercLog->log("%s,invalid LP,%s,bq %d,bp %g,ap %g,aq %d,lp %g,lq %d,stp %g", m_pEnv->m_strategyName,ID(),m_pMD->m_BQ,m_pMD->m_BP,m_pMD->m_AP,m_pMD->m_AQ,m_pMD->m_LP,m_pMD->m_LQ,price); }
        }
        void notifyOpenTrade(int volume, double price)
        {
            if (m_pEnv->m_isBacktest>0) { price = volume>0?(price+m_pEnv->m_slipTics*m_pMD->m_tick):(price-m_pEnv->m_slipTics*m_pMD->m_tick); }
            m_pSignal->m_pos += volume;
            m_commission += m_pMD->m_feePerLot*abs(volume);
        }
        void settle() { if (m_settled) { return; } }
        void settled() {   m_settled = true; }
        bool isSettled() { return m_settled; }
        int id() { return m_id; }
        int prodRef() { return m_pInstrument->getProduct()->getProductRef(); }
        const char *ID() { return m_pInstrument->getInstrumentID(); }
        const char *productID() { return m_pInstrument->getProductID(); }
        const char *exchangeID(void) const { return m_pInstrument->getExchangeID(); }
        const CInstrument *pInstrument() { return m_pInstrument; }
        bool hasOrder() { return m_pMercPos->hasOrder(); }
        int mercPos() { return m_pMercPos->m_position; }
        double commission() { return m_commission; }
        int ED() { return m_pMD->m_expirationDate; }
        int EDC() { return m_pMD->m_expirationDayCount; }
        int EM() { return m_pMD->m_expirationMonth; }
        int preOI() { return m_pMD->m_preOpenInterest; }
        double tick() { return m_pMD->m_tick; }
        double multiply() { return m_pMD->m_multiply; }
        int BQ() { return m_pMD->m_BQ; }
        int AQ() { return m_pMD->m_AQ; }
        double BP() { return m_pMD->m_BP; }
        double AP() { return m_pMD->m_AP; }
        double LP() { return m_pMD->m_LP; }
        int LV() { return m_pMD->m_LV; }
        int LBQ() { return m_pMD->m_LBQ; }
        int LAQ() { return m_pMD->m_LAQ; }
        double LBP() { return m_pMD->m_LBP; }
        double LAP() { return m_pMD->m_LAP; }
        double LLP() { return m_pMD->m_LLP; }
        int LLV() { return m_pMD->m_LLV; }
        double GAP() { return m_pMD->m_GAP; }
        double GAPEMA() { return m_pMD->m_GAPEMA; }
        double upperLimitPrice() { return m_pMD->m_upperLimitPrice; }
        double lowerLimitPrice() { return m_pMD->m_lowerLimitPrice; }
        double lastTheo(void) { return m_pSignal->m_theoLst; }
        CSignalAE *signalAE(void) { return m_pSignal; }
        bool staticError() { return m_staticError; }
        void subscribeTask(int workerID,int taskID) { m_pForceTasks[workerID] = taskID; }

        double margin()
        {
            double marginPerLot = m_pMD->m_marginPerLot;
            if (m_pEnv->m_mrgnRt > 0.0)
                marginPerLot = m_pMD->m_preClosePrice * multiply() * m_pEnv->m_mrgnRt;
            return marginPerLot*abs(mercPos());
        }
        double marginPerLot()
        {
            double marginPerLot = m_pMD->m_marginPerLot;
            if (m_pEnv->m_mrgnRt > 0.0)
                marginPerLot = m_pMD->m_preClosePrice * multiply() * m_pEnv->m_mrgnRt;
            return marginPerLot;
        }

        void unsubscribeTask(int workerID)
        {
            if (m_pForceTasks.find(workerID)!=m_pForceTasks.end())
            {
                m_pForceTasks.erase(workerID);
            }
        }
    };

    class CForceTask
    {
    private:
        int m_workerID;
        const CStratsEnvAE *m_pEnv;
        bool m_hasTask;
    public:
        int m_forceOrderType;
        int m_forceOrderDirection;
        double m_forceOrderPrice;
        int m_forceOrderVolume;
        int m_taskID;
        int m_spreadID;
        int m_orderID;
        int m_stopTS;
        CFutureExtentionAE *m_pLeg;
        int m_legID;
        int m_expVlm;
        int m_trdVlm;
        unsigned m_triedCount;
        unsigned m_triedCountBetweenMD;
        unsigned m_triedCountAfterMD;
        unsigned m_errorCount;
        bool m_timeOut;
        CForceTask(int id,const CStratsEnvAE *pEnv)
        {
            m_workerID=id;
            m_pEnv=pEnv;
            reset();
        }
        void reset()
        {
            m_hasTask=false;
            m_forceOrderType=(m_pEnv->m_forceOrderWaitTime>=0) ? ODT_Limit : ODT_FAK;
            m_forceOrderDirection=0;
            m_forceOrderPrice=0.0;
            m_forceOrderVolume=0;
            m_taskID=m_spreadID=m_orderID=m_stopTS=-1;
            m_pLeg=NULL;
            m_expVlm=m_trdVlm=0;
            m_triedCount=m_triedCountBetweenMD=m_triedCountAfterMD=m_errorCount=0;
            m_timeOut=false;
        }
        void init(int taskID)
        {
            m_taskID=taskID;
            m_hasTask=true;
        }
        void start(int spreadID, CFutureExtentionAE *pFrcLeg, int frcLegID, int expVlm)
        {
            m_spreadID=spreadID;
            m_pLeg = pFrcLeg;
            m_legID = frcLegID;
            m_expVlm=expVlm;
        }
        int adjTicks()
        {
            int adjTic = m_pEnv->m_startPriceAdjustTicks;
            adjTic += m_triedCountBetweenMD*m_pEnv->m_stepPriceAdjustTicksBetweenMD;
            adjTic += m_triedCountAfterMD*m_pEnv->m_stepPriceAdjustTicksAfterMD;
            return adjTic;
        }
        void prepareForceOrder()
        {
            int volume = remainVlm();
            double price = 0;
            double adjPrice = adjTicks() * m_pLeg->tick();
            if (volume > 0) { price = std::min(m_pLeg->upperLimitPrice(),m_pLeg->AP()+adjPrice); }
            else { price = std::max(m_pLeg->lowerLimitPrice(),m_pLeg->BP()-adjPrice); }
            m_forceOrderType=(m_pEnv->m_forceOrderWaitTime>=0) ? ODT_Limit : ODT_FAK;
            m_forceOrderDirection=(volume > 0) ? D_Buy : D_Sell;
            m_forceOrderPrice=price;
            m_forceOrderVolume=abs(volume);
        }
        bool hasTask() { return m_hasTask; }
        bool hasOrder() { return (m_orderID >= 0); }
        int workerID() { return m_workerID; }
        int orderID() { return m_orderID; }
        int taskID() { return m_taskID; }
        int spreadID() { return m_spreadID; }
        CFutureExtentionAE *pFuture() { return m_pLeg; }
        const CInstrument *pInstrument() { return m_pLeg->pInstrument(); }
        int remainVlm() { return (m_expVlm-m_trdVlm); }
        void notifyOrderSent(int orderID) { m_orderID=orderID; m_triedCount++; m_triedCountBetweenMD++; }
        void notifyOrderSendFailed() { m_orderID=-1; m_errorCount++; }
        void notifyOrderFinish(int volume) { m_orderID=-1; m_trdVlm += volume; }
        void notifyOrderFailed() { m_orderID=-1; m_errorCount++; }
        void notifyMD()
        {
            m_triedCountBetweenMD=0;
            if (!hasOrder()) { m_triedCountAfterMD++; } // ensure this MD will trigger order
        }
        void notifyTimerSet(int timeStamp) { m_stopTS = timeStamp; }
        bool checkTimeout(int timeStamp)
        {
            if (!m_timeOut && m_stopTS>=0 && (timeStamp+5)>=m_stopTS) { m_timeOut=true; }
            return m_timeOut;
        }
        int updateConstrain()
        {
            int constrain = 0;
            if (hasOrder()) { constrain = -1; }
            else if (remainVlm() == 0) { constrain = -2; }
            else if (m_triedCountBetweenMD >= m_pEnv->m_maxTriedCountBetweenMD) { constrain = 1; }
            else if (m_errorCount >= m_pEnv->m_maxErrorCount) { constrain = 2; }
            else if (m_triedCount >= m_pEnv->m_maxTriedCount) { constrain = 2; }
            else if (m_timeOut) { constrain = 2; }
            return constrain;
        }
        bool needResendOrder() { int constrain = updateConstrain(); return (constrain == 0); }
        bool tryStop() { int constrain = updateConstrain(); return (constrain == -2 || constrain == 2); }
        void stop() { reset(); }
    };

    class CForceTaskManager
    {
    private:
        int m_id;
    public:
        int m_forceTaskCount;
        std::map<int, CForceTask*> m_pForceTasks;
        CForceTaskManager(int id,const CStratsEnvAE *pEnv,int maxWorker)
        {
            m_id=id;
            m_forceTaskCount=0;
            for (int i=0;i<maxWorker;i++) { m_pForceTasks[i] = new CForceTask(i,pEnv); }
        }
        CForceTask *getWorker()
        {
            for (auto& it : m_pForceTasks)
            {
                if (!it.second->hasTask())
                {
                    it.second->init(m_forceTaskCount);
                    m_forceTaskCount++;
                    return it.second;
                }
            }
            return NULL;
        }
        CForceTask *get(int workerID)
        {
            if (m_pForceTasks.find(workerID)!=m_pForceTasks.end()) { return m_pForceTasks[workerID]; }
            return NULL;
        }
    };

    class CSpreadExec
    {
    private:
        int m_spreadID;
        const CStratsEnvAE *m_pEnv;
        bool m_isProcessing;
    public:
        std::string m_sprdNm;
        std::vector<CFutureExtentionAE *> m_pLegs = {};
        std::vector<double> m_coefs = {};
        std::vector<double> m_exeCoefs = {};
        CFutureExtentionAE *m_pTryLeg;
        std::vector<CFutureExtentionAE *>m_pFrcLegs = {};
        std::map<int,CForceTask*> m_pForceTasks;

		double m_sprdMulti = 0.0;

        int m_tryLegID;
        int m_spreadExpVlm;
        int m_spreadTrdVlm;
        int m_tryExpVlm;
        int m_tryTrdVlm;
        std::map<int, int> m_expVlmMap = {};
        std::map<int, int> m_trdVlmMap = {};
        double m_tryAvgPrice;
        std::map<int, double> m_avgPriceMap = {};
        double m_spreadAvgPrice;
        double m_sprdExeAvgPr;
        double m_sprdTgtPr;

        int m_tryOrderID;
        int m_tryOrderType;
        int m_tryOrderDirection;
        int m_tryOrderVolume;
        double m_tryOrderPrice;
        std::map<int,int> m_remainPositions;

        CSpreadExec(int id,const CStratsEnvAE *pEnv)
        {
            m_spreadID=id;
            m_pEnv=pEnv;
            m_isProcessing=false;
            m_pTryLeg=NULL;
            m_tryLegID=0;
            m_spreadExpVlm=m_spreadTrdVlm=m_tryExpVlm=m_tryTrdVlm=0;
            m_tryAvgPrice=m_spreadAvgPrice=m_sprdExeAvgPr=0.0;
            m_tryOrderID=-1;
            m_sprdMulti = 0.0;
        }
        void start(int expVlm, int tryLegID)
        {
            if (expVlm==0)
            {
                stop();
            }
            m_spreadExpVlm = expVlm;
            m_tryLegID = tryLegID;
            for (int i=0; i<m_pLegs.size(); i++)
            {
                if (i == m_tryLegID)
                {
                    m_pTryLeg = m_pLegs.at(i);
                    m_tryExpVlm = m_spreadExpVlm * m_exeCoefs.at(i);
                }
                else if (m_exeCoefs.at(i) != 0)
                    m_pFrcLegs.push_back(m_pLegs.at(i));

                m_expVlmMap[i] = m_spreadExpVlm * m_exeCoefs.at(i);
                m_trdVlmMap[i] = 0;
                m_avgPriceMap[i] = 0;
            }

            m_isProcessing = true;
        }
        int getLegId(int instRef)
        {
            for (int i=0; i<m_pLegs.size(); i++) { if (instRef == m_pLegs.at(i)->id()) return i; }
            return -1;
        }
        void prepareTryOrder()
        {
            double price = 0;
            double adjPrice = m_pTryLeg->tick()*m_pEnv->m_tryOrderPriceAdjustTicks;
            if (m_tryExpVlm > 0) { price = std::min(m_pTryLeg->upperLimitPrice(), m_pTryLeg->AP()+adjPrice); }
            else { price = std::max(m_pTryLeg->lowerLimitPrice(), m_pTryLeg->BP()-adjPrice); }
            m_tryOrderType = (m_pEnv->m_tryOrderWaitTime>=0) ? ODT_Limit : ODT_FAK;
            m_tryOrderDirection = (m_tryExpVlm > 0) ? D_Buy : D_Sell;
            m_tryOrderPrice = price;
            if (m_tryTrdVlm == 0)
                m_tryOrderVolume = abs(m_tryExpVlm);
            else
                m_tryOrderVolume = abs(calcLegVlm(m_tryLegID, calcSprdVlmCeil(m_tryLegID, m_tryTrdVlm))) - abs(m_tryTrdVlm);
        }
        void stop()
        {
            m_spreadExpVlm=m_spreadTrdVlm=m_tryExpVlm=m_tryTrdVlm=0;
            m_tryAvgPrice=m_spreadAvgPrice=m_sprdExeAvgPr=0.0;
            m_tryOrderID=-1;
            m_expVlmMap.clear();
            m_trdVlmMap.clear();
            m_avgPriceMap.clear();
            m_isProcessing = false;
        }
        bool tryStop()
        {
            int pendingVlmZero = true;
            for (int i=0; i<m_pLegs.size(); i++)
            {
                pendingVlmZero = pendingVlmZero && (pendingVlm(i) == 0);
                /* // try leg must trade volume even to coef */
                /* if (i == m_tryLegID && evnLegVlm(i, m_trdVlmMap[i]) != m_trdVlmMap[i]) */
                /*     pendingVlmZero = false; */
                if (!pendingVlmZero)
                    g_pMercLog->log("[CSpreadExec.tryStop|FAILED]%s,pendingVlm,%d,legID,%d", m_sprdNm.c_str(), pendingVlm(i), i);
            }
            bool tryStopRes = (m_tryOrderID<0 && pendingVlmZero && m_pForceTasks.size()==0);
            return tryStopRes;
        }
        void tryOrderSent(int orderID) { m_tryOrderID = orderID; }
        void tryOrderSendFailed() { m_tryOrderID = -1; }
        void tryOrderFailed() { m_tryOrderID = -1; }
        void tryOrderFinished() { m_tryOrderID = -1; }

        int calcLegVlm(int legID, int sprdVlm) { int coef = m_exeCoefs.at(legID); return sprdVlm * coef; }

        int calcSprdVlm(int legID, int legVlm)
        {
            if (m_exeCoefs.at(legID) == 0)
                return 0;

            double sprdVlm = double(legVlm) / m_exeCoefs.at(legID);
            sprdVlm = sprdVlm > 0? flrPrice(sprdVlm, 1): ceilPrice(sprdVlm, 1);
            return sprdVlm;
        }

        int evnLegVlm(int legID, int vlm)
        {
            int sprdVlm = calcSprdVlm(legID, vlm);
            int legVlm = calcLegVlm(legID, sprdVlm);
            return legVlm;
        }
 
        int calcSprdVlmCeil(int legID, int legVlm)
        {
            if (m_exeCoefs.at(legID) == 0)
                return 0;

            double sprdVlm = double(legVlm) / m_exeCoefs.at(legID);
            sprdVlm = sprdVlm > 0? ceilPrice(sprdVlm, 1): flrPrice(sprdVlm, 1);
            return sprdVlm;
        }
 
        int evnLegVlmCeil(int legID, int vlm)
        {
            int sprdVlm = calcSprdVlmCeil(legID, vlm);
            int legVlm = calcLegVlm(legID, sprdVlm);
            return legVlm;
        }

        void tryOrderTraded(int volume,double price)
        {
            if (m_tryTrdVlm!=0)
            {
                m_tryAvgPrice = (m_tryAvgPrice*m_tryTrdVlm+price*volume) / (m_tryTrdVlm+volume);
                m_avgPriceMap[m_tryLegID] = (m_avgPriceMap[m_tryLegID] * m_trdVlmMap[m_tryLegID] + price*volume) / (m_trdVlmMap[m_tryLegID] + volume);
            }
            else
            {
                m_tryAvgPrice = price;
                m_avgPriceMap[m_tryLegID] = price;
            }
            m_tryTrdVlm += volume;
            m_trdVlmMap[m_tryLegID] += volume;
        }
        void forceOrderTraded(int legID, int volume,double price)
        {
            if (m_trdVlmMap[legID]!=0)
            {
                m_avgPriceMap[legID] = (m_avgPriceMap[legID] * m_trdVlmMap[legID] + price*volume) / (m_trdVlmMap[legID] + volume);
            }
            else
            {
                m_avgPriceMap[legID] = price;
            }
            m_trdVlmMap[legID] += volume;
        }
        void forceOrderFinished()
        {
        }
        int pendingVlm(int legID)
        {
            int tryTrdVlm2SprdVlm = calcSprdVlmCeil(m_tryLegID, m_tryTrdVlm);
            // in case legID not trd yet
            if (m_trdVlmMap.find(legID) == m_trdVlmMap.end())
                m_trdVlmMap[legID] = 0;
            return tryTrdVlm2SprdVlm * m_exeCoefs.at(legID) - m_trdVlmMap[legID];
        }

        int pendingTask(int legID)
        {
            for (auto pFrcTask: m_pForceTasks)
            {
                if (pFrcTask.second->m_legID == legID)
                {
                    return pFrcTask.second->taskID();
                }
            }
            return -1;
        }

        bool isBalanced()
        {
            for (int legID=0; legID<m_pLegs.size(); legID++)
            {
                if (m_exeCoefs.at(legID) == 0)
                    continue;
                
                int vlm = m_trdVlmMap[legID];
                int sprdVlm = calcSprdVlm(legID, vlm);
                int legVlm = calcLegVlm(legID, sprdVlm);
                if (sprdVlm != m_spreadTrdVlm || legVlm != vlm)
                    return false;
            }
            return true;
        }
        int calcSpreadTrdVolume()
        {
            m_spreadTrdVlm = INT_MAX;
            m_spreadAvgPrice = 0.0;
            m_sprdExeAvgPr = 0.0;
            for (int i=0; i<m_pLegs.size(); i++)
            {
                if (m_exeCoefs.at(i) != 0)
                    m_spreadTrdVlm = std::min(m_spreadTrdVlm, abs(calcSprdVlm(i, m_trdVlmMap[i])));
            }
            for (int i=0; i<m_pLegs.size(); i++)
            {
                if (m_exeCoefs.at(i) != 0)
                {
                    m_spreadAvgPrice += m_coefs.at(i) * m_avgPriceMap[i];
                }
                else
                {
                    double mktPr;
                    if (m_coefs.at(i) * m_spreadExpVlm > 0)
                        mktPr = m_coefs.at(i) > 0? m_pLegs.at(i)->AP(): m_pLegs.at(i)->BP();
                    else if (m_coefs.at(i) * m_spreadExpVlm < 0)
                        mktPr = m_coefs.at(i) > 0? m_pLegs.at(i)->BP(): m_pLegs.at(i)->AP();
                    else
                        mktPr = (m_pLegs.at(i)->BP() + m_pLegs.at(i)->AP()) / 2.0;

                    m_spreadAvgPrice += m_coefs.at(i) * mktPr;
                }

                double legMultiCoef = m_pLegs.at(i)->multiply() / m_sprdMulti;
                m_sprdExeAvgPr += m_exeCoefs.at(i) * m_avgPriceMap[i] * legMultiCoef;
            }
            m_spreadTrdVlm = m_spreadExpVlm < 0? -m_spreadTrdVlm: m_spreadTrdVlm;

            if (m_spreadTrdVlm == 0)
                m_spreadAvgPrice = m_sprdExeAvgPr = 0.0;

            if (!isBalanced())
            {
                //TODO: handle history remainPosition
                addRemainPositions();
            }
            return m_spreadTrdVlm;
        }
        void addRemainPositions()
        {
            int tgtSprdVlm = 0;
            for (int legID=0; legID<m_pLegs.size(); legID++)
            {
                int vlm = m_trdVlmMap[legID];
                int sprdVlm = calcSprdVlmCeil(legID, vlm); 
                if (abs(sprdVlm) > abs(tgtSprdVlm))
                {
                    tgtSprdVlm = sprdVlm;
                }
            }

            for (int legID=0; legID<m_pLegs.size(); legID++)
            {
                int vlm = m_trdVlmMap[legID];
                int tgtVlm = calcLegVlm(legID, tgtSprdVlm);
                // persist remainPositions
                if (vlm != tgtVlm)
                {
                    m_remainPositions[legID] += tgtVlm - vlm;
                }
            }
        }
        void reduceRemainPositions(int legID, int pos)
        {
            if (m_remainPositions.find(legID) != m_remainPositions.end())
            {
                m_remainPositions[legID] += pos;
                if (m_remainPositions[legID]==0)
                {
                    m_remainPositions.erase(legID);
                }
            }
        }
        void subscribeTask(CForceTask *pTask, int legID)
        {
            m_expVlmMap[legID] = pTask->m_expVlm;
            m_pForceTasks[pTask->taskID()]=pTask;
        }
        CForceTask *getTask(int taskID)
        {
            if (m_pForceTasks.find(taskID) != m_pForceTasks.end())
            {
                return m_pForceTasks[taskID];
            }
            return NULL;
        }
        void unsubscribeTask(int taskID)
        {
            if (m_pForceTasks.find(taskID) != m_pForceTasks.end())
            {
                m_pForceTasks.erase(taskID);
            }
            return;
        }
        int tryID() { return m_pTryLeg->id(); }
        int forceID(int legID) { return m_pLegs.at(legID)->id(); }
        const CInstrument *pTryInstrument() { return m_pTryLeg->pInstrument(); }
        const CInstrument *pForceInstrument(int legID) { return m_pLegs.at(legID)->pInstrument(); }
        bool isProcessing() { return m_isProcessing; }
        int spreadID() { return m_spreadID; }
    };

    class CSpreadExtentionAE
    {
    private:
        int m_id;
        IMercStrategy *m_pStrategy;
        CFuzzySort *m_pFuzzySorter;
        char m_buffer[500];
    public:
        const CStratsEnvAE *m_pEnv;
        std::vector<CFutureExtentionAE *> m_pLegs;
        std::vector<double> m_coefs, m_exeCoefs;
        std::string m_sprdNm;
        double m_sprdMulti = 0.0;
        CSpreadExec* m_pSpreadExec;
        CSpreadSignal *m_pSignal;
        int m_EDC, m_EDC2, m_ltdc;

        int m_pos;
        int m_selfConstrain, m_internalConstrain, m_externalConstrain;
        int m_manTrdRt;

        double m_tick;
        int m_ticDecPlc;
        double m_ticM;
        double m_multiply;
        double m_marginPerPair;
        int m_snapSecond;
        int m_maxTradeSize;
        double m_maxAmt;
        double m_mrgnPct;
        double m_hdMrgnPct;
        double m_ttlMrgn;
        
        bool m_isPreFlush;
        int m_spAQ;
        int m_spBQ;
        double m_spMP;
        double m_spAP;
        double m_spBP;
        double m_exeSpAP;
        double m_exeSpBP;
        double m_spLAP;
        double m_spLBP;
        double m_spAvg;
        double m_refMid;
        int m_manSprdMaxLot;
        int m_stepSize;

        int m_sprdNmPos = 0;
        double m_buy;
        double m_sell;
        double m_refBuy;
        double m_refSell;
        int m_bidQ;
        int m_askQ;
        
        // Dynamic grid strategy parameters
        double m_exitInterval = 0.0;                 // Exit interval for taking profit
        double m_minEntryInterval = 0.0;             // Minimum entry interval between grid levels
        double m_minDynamic = 1.0;                   // Minimum dynamic factor
        double m_maxDynamic = 3.0;                   // Maximum dynamic factor
        double m_widenThreshold = 0.3;               // Profitable rate threshold to widen grid
        double m_narrowThreshold = 0.7;              // Profitable rate threshold to narrow grid
        double m_widenStep = 1.2;                    // Step size for widening/narrowing
        int m_minOps = 10;                           // Minimum operations before adjusting
        double m_reduceRatio = 0.6;                  // Ratio of position to reduce in risk mode
        double m_maxLeverage = 20.0;                 // Maximum leverage allowed
        int m_maxGridLevels = 500;                   // Maximum number of grid levels
        
        // Boundary tracking for dynamic updates
        double m_arbitrageLower = 0.0;
        double m_arbitrageUpper = 0.0;
        double m_riskLower = 0.0;
        double m_riskUpper = 0.0;
        
        // Window sizes for boundary calculation (in days)
        int m_arbitrageN = 120;                      // Days for arbitrage boundary
        int m_riskN = 180;                           // Days for risk boundary
        int m_updateIntervalMinutes = 15;            // Update interval in minutes

        double m_gapThreshold;
        int m_triggerVolume;
        double m_triggerPrice;
        int m_lastTrdVlm;
        double m_lastTrdPrice;

        CMercStrategyStatus *m_pSprdIdxStatus;
        CMercStrategyStatus *m_pEMAStatus;
        CMercStrategyStatus *m_pPrMdlOpnStatus;
        CMercStrategyStatus *m_pPrMdlClsStatus;
        CMercStrategyStatus *m_pPrMdlParamStatus;
        CMercStrategyStatus *m_pPrMdlParamTicStatus;
        CMercStrategyStatus *m_pMDStatus;
        CMercStrategyStatus *m_pBollStatus;
        CMercStrategyStatus *m_pTrdStatus;
        CMercStrategyStatus *m_pPnlStatusArb;
        CMercStrategyStatus *m_pPnlStatusVw;
        CMercStrategyFlow *m_pTradeFlow;

        CSpreadExtentionAE(int id,IMercStrategy *pStrategy,const CStratsEnvAE *pEnv,CFuzzySort *pFuzzySorter)
        {
            m_id=id;
            m_pStrategy=pStrategy;
            m_pEnv=pEnv;
            m_pFuzzySorter=pFuzzySorter;
            m_pSignal = NULL;
            m_pSpreadExec=new CSpreadExec(id,pEnv);
            m_EDC=m_EDC2=m_pos=0;
            m_selfConstrain=m_internalConstrain=m_externalConstrain=m_manTrdRt=0;
            m_tick=m_multiply=m_marginPerPair=0.0;
            m_ticDecPlc = 0;
            m_ticM = 1.0;
            m_stepSize=m_snapSecond=m_maxTradeSize=0;

            m_maxAmt = m_mrgnPct = m_hdMrgnPct = 0.0;
            m_ttlMrgn = 0.0;
            m_isPreFlush=false;
            m_spAQ=m_spBQ=0;
            m_spMP = 0.0;
            m_spAP=m_spBP=m_spLAP=m_spLBP=0.0;
            m_exeSpAP = m_exeSpBP = 0.0;
            m_spAvg=m_refMid=DBL_MAX;

            m_buy=m_sell=0.0;
            m_refBuy=-DBL_MAX;
            m_refSell=DBL_MAX;
            m_bidQ = m_askQ = 0;
            m_gapThreshold=0.0;
            m_triggerVolume=m_lastTrdVlm=0;
            m_triggerPrice=m_lastTrdPrice=0.0;

            m_pSprdIdxStatus=NULL;
            m_pEMAStatus=NULL;
            m_pPrMdlOpnStatus=NULL;
            m_pPrMdlClsStatus=NULL;
            m_pPrMdlParamStatus=NULL;
            m_pPrMdlParamTicStatus=NULL;
            m_pMDStatus=NULL;
            m_pBollStatus=NULL;
            m_pTrdStatus=NULL;
            m_pPnlStatusArb=NULL;
            m_pPnlStatusVw=NULL;
            m_pTradeFlow=NULL;
        }
        void refreshPrMdlOpnStatus()
        {
            if (m_pPrMdlOpnStatus==NULL)
            {
                int idxCoef = 10000;
                int leaderID = m_pLegs.size() > 0? m_pLegs.at(0)->id(): -1;
                int laggerID = (m_pLegs.size() > 1? m_pLegs.at(1)->id(): -1) * idxCoef;
                for (int i=2; i<m_pLegs.size(); i++) { idxCoef *= 10; laggerID += m_pLegs.at(1)->id() * idxCoef; }
                m_pPrMdlOpnStatus=m_pStrategy->createStrategyStatus(6, leaderID, laggerID);

            }
            m_pPrMdlOpnStatus->IntValue[0] = roundPrice(m_pSignal->m_refMid * m_ticM, m_tick);
            m_pPrMdlOpnStatus->IntValue[1] = 0;
            m_pPrMdlOpnStatus->IntValue[2] = 0;
            m_pPrMdlOpnStatus->IntValue[3] = m_pos >= 0? m_pSignal->m_stepSize: -m_pSignal->m_stepSize;
            m_pPrMdlOpnStatus->FloatValue[0] = 0.0;
            m_pPrMdlOpnStatus->FloatValue[1] = m_pos;

            m_pStrategy->refreshStrategyStatus(m_pPrMdlOpnStatus);
        }
        void refreshPrMdlClsStatus()
        {
            if (m_pPrMdlClsStatus==NULL)
            {
                int idxCoef = 10000;
                int leaderID = m_pLegs.size() > 0? m_pLegs.at(0)->id(): -1;
                int laggerID = (m_pLegs.size() > 1? m_pLegs.at(1)->id(): -1) * idxCoef;
                for (int i=2; i<m_pLegs.size(); i++) { idxCoef *= 10; laggerID += m_pLegs.at(1)->id() * idxCoef; }
                m_pPrMdlClsStatus=m_pStrategy->createStrategyStatus(7, leaderID, laggerID);

            }
            m_pPrMdlClsStatus->IntValue[0] = m_pos;
            m_pPrMdlClsStatus->IntValue[1] = 0;
            m_pPrMdlClsStatus->IntValue[2] = 0;
            m_pPrMdlClsStatus->IntValue[3] = 0;
            m_pPrMdlClsStatus->FloatValue[0] = 0;
            m_pPrMdlClsStatus->FloatValue[1] = roundPrice(m_pSignal->m_atp, m_tick);
            m_pStrategy->refreshStrategyStatus(m_pPrMdlClsStatus);
        }
        void refreshPrMdlParamStatus()
        {
            if (m_pPrMdlParamStatus==NULL)
            {
                int idxCoef = 10000;
                int leaderID = m_pLegs.size() > 0? m_pLegs.at(0)->id(): -1;
                int laggerID = (m_pLegs.size() > 1? m_pLegs.at(1)->id(): -1) * idxCoef;
                for (int i=2; i<m_pLegs.size(); i++) { idxCoef *= 10; laggerID += m_pLegs.at(1)->id() * idxCoef; }
                m_pPrMdlParamStatus=m_pStrategy->createStrategyStatus(8, leaderID, laggerID);
            }
            
            m_pPrMdlParamStatus->IntValue[0] = 0;
            m_pPrMdlParamStatus->IntValue[1] = 0;
            m_pPrMdlParamStatus->IntValue[2] = 0;
            m_pPrMdlParamStatus->IntValue[3] = 0;
            m_pPrMdlParamStatus->FloatValue[0] = 0.0;
            m_pPrMdlParamStatus->FloatValue[1] = 0.0;
            m_pStrategy->refreshStrategyStatus(m_pPrMdlParamStatus);
        }
        void refreshPrMdlParamTicStatus()
        {
            if (m_pPrMdlParamTicStatus==NULL)
            {
                int idxCoef = 10000;
                int leaderID = m_pLegs.size() > 0? m_pLegs.at(0)->id(): -1;
                int laggerID = (m_pLegs.size() > 1? m_pLegs.at(1)->id(): -1) * idxCoef;
                for (int i=2; i<m_pLegs.size(); i++) { idxCoef *= 10; laggerID += m_pLegs.at(1)->id() * idxCoef; }
                m_pPrMdlParamTicStatus=m_pStrategy->createStrategyStatus(9, leaderID, laggerID);
            }
            m_pPrMdlParamTicStatus->IntValue[0] = 0;
            m_pPrMdlParamTicStatus->IntValue[1] = 0;
            m_pPrMdlParamTicStatus->IntValue[2] = 0;
            m_pPrMdlParamTicStatus->IntValue[3] = 0;
            m_pPrMdlParamTicStatus->FloatValue[0] = 0.0;
            m_pPrMdlParamTicStatus->FloatValue[1] = 0.0;
            m_pStrategy->refreshStrategyStatus(m_pPrMdlParamTicStatus);
        }
        void refreshEMAStatus()
        {
            if (m_pEMAStatus==NULL)
            {
                int idxCoef = 10000;
                int leaderID = m_pLegs.size() > 0? m_pLegs.at(0)->id(): -1;
                int laggerID = (m_pLegs.size() > 1? m_pLegs.at(1)->id(): -1) * idxCoef;
                for (int i=2; i<m_pLegs.size(); i++) { idxCoef *= 10; laggerID += m_pLegs.at(1)->id() * idxCoef; }
                m_pEMAStatus=m_pStrategy->createStrategyStatus(10, leaderID, laggerID);
            }
            m_pEMAStatus->IntValue[0] = m_EDC;
            m_pEMAStatus->IntValue[1] = 0;
            m_pEMAStatus->IntValue[2] = 0;
            m_pEMAStatus->IntValue[3] = m_selfConstrain;
            m_pEMAStatus->FloatValue[0] = roundPrice(m_pSignal->m_refMid, m_tick);
            m_pEMAStatus->FloatValue[1] = 0.0;
            m_pStrategy->refreshStrategyStatus(m_pEMAStatus);
        }
        void refreshMDStatus()
        {
            if (m_pMDStatus==NULL)
            {
                int idxCoef = 10000;
                int leaderID = m_pLegs.size() > 0? m_pLegs.at(0)->id(): -1;
                int laggerID = (m_pLegs.size() > 1? m_pLegs.at(1)->id(): -1) * idxCoef;
                for (int i=2; i<m_pLegs.size(); i++) { idxCoef *= 10; laggerID += m_pLegs.at(1)->id() * idxCoef; }
                m_pMDStatus=m_pStrategy->createStrategyStatus(11, leaderID, laggerID);
            }
            m_pMDStatus->IntValue[0]=m_pSignal->m_sprdBQ;
            m_pMDStatus->FloatValue[0]=m_pSignal->m_sprdBP;
            m_pMDStatus->FloatValue[1]=m_pSignal->m_sprdAP;
            m_pMDStatus->IntValue[1]=m_pSignal->m_sprdAQ;
            m_pMDStatus->IntValue[2]=int(roundPrice(m_tick / m_pLegs.at(0)->m_pSignal->m_theoLst * 10000, 1.0));
            m_pMDStatus->IntValue[3]=0;
            m_pStrategy->refreshStrategyStatus(m_pMDStatus);
        }
        void refreshBollStatus()
        {
            if (m_pBollStatus==NULL)
            {
                int idxCoef = 10000;
                int leaderID = m_pLegs.size() > 0? m_pLegs.at(0)->id(): -1;
                int laggerID = (m_pLegs.size() > 1? m_pLegs.at(1)->id(): -1) * idxCoef;
                for (int i=2; i<m_pLegs.size(); i++) { idxCoef *= 10; laggerID += m_pLegs.at(1)->id() * idxCoef; }
                m_pBollStatus=m_pStrategy->createStrategyStatus(12, leaderID, laggerID);
            }
            m_pBollStatus->IntValue[0]=m_pos;
            m_pBollStatus->IntValue[1]=m_manSprdMaxLot;
            m_pBollStatus->IntValue[2]=int(m_ttlMrgn / 1000.0);
            m_pBollStatus->IntValue[3]=int(m_maxAmt / 1000.0);
            m_pBollStatus->FloatValue[0]=m_buy;
            m_pBollStatus->FloatValue[1]=m_sell;
            m_pStrategy->refreshStrategyStatus(m_pBollStatus);
        }
        void refreshTrdStatus()
        {
            if (m_pTrdStatus==NULL)
            {
                int idxCoef = 10000;
                int leaderID = m_pLegs.size() > 0? m_pLegs.at(0)->id(): -1;
                int laggerID = (m_pLegs.size() > 1? m_pLegs.at(1)->id(): -1) * idxCoef;
                for (int i=2; i<m_pLegs.size(); i++) { idxCoef *= 10; laggerID += m_pLegs.at(1)->id() * idxCoef; }
                m_pTrdStatus=m_pStrategy->createStrategyStatus(13, leaderID, laggerID);
            }

            m_pTrdStatus->IntValue[0] = m_pos;
            m_pTrdStatus->FloatValue[0] = roundPrice(m_pSignal->m_atp, m_tick);
            m_pTrdStatus->FloatValue[1] = roundPrice(m_pSignal->m_awp, m_tick);
            m_pTrdStatus->IntValue[1] = m_pos >= 0? m_pSignal->m_stepSize: -m_pSignal->m_stepSize;
            m_pTrdStatus->IntValue[2] = m_stepSize;
            m_pTrdStatus->IntValue[3] = m_manSprdMaxLot;
            m_pStrategy->refreshStrategyStatus(m_pTrdStatus);
        }

        void refreshPnlStatus()
        {
            if (m_pPnlStatusArb==NULL)
            {
                int idxCoef = 10000;
                int leaderID = m_pLegs.size() > 0? m_pLegs.at(0)->id(): -1;
                int laggerID = (m_pLegs.size() > 1? m_pLegs.at(1)->id(): -1) * idxCoef;
                for (int i=2; i<m_pLegs.size(); i++) { idxCoef *= 10; laggerID += m_pLegs.at(1)->id() * idxCoef; }
                m_pPnlStatusArb=m_pStrategy->createStrategyStatus(14, leaderID, laggerID);
            }
            settle();
            m_pPnlStatusArb->IntValue[0]=0;
            m_pPnlStatusArb->IntValue[1]=int(roundPrice(m_pSignal->m_diPnl, 1));
            m_pPnlStatusArb->IntValue[2]=int(roundPrice(m_pSignal->m_dnPnl, 1));
            m_pPnlStatusArb->IntValue[3]=int(roundPrice(m_pSignal->m_pnl, 1));
            m_pPnlStatusArb->FloatValue[0] = m_tick;
            m_pPnlStatusArb->FloatValue[1] = m_ticM;
            m_pStrategy->refreshStrategyStatus(m_pPnlStatusArb);
        }
        void refreshSprdIdxStatus()
        {
            if (m_pSprdIdxStatus==NULL)
            {
                int idxCoef = 10000;
                int leaderID = m_pLegs.size() > 0? m_pLegs.at(0)->id(): -1;
                int laggerID = (m_pLegs.size() > 1? m_pLegs.at(1)->id(): -1) * idxCoef;
                for (int i=2; i<m_pLegs.size(); i++) { idxCoef *= 10; laggerID += m_pLegs.at(1)->id() * idxCoef; }
                m_pSprdIdxStatus=m_pStrategy->createStrategyStatus(15, leaderID, laggerID);

            }
            m_pSprdIdxStatus->IntValue[0] = m_id;
            m_pStrategy->refreshStrategyStatus(m_pSprdIdxStatus);
        }
        void refreshAllStatus()
        {
            refreshPrMdlOpnStatus();
            refreshPrMdlClsStatus();
            refreshPrMdlParamStatus();
            refreshPrMdlParamTicStatus();
            refreshEMAStatus();
            refreshSprdIdxStatus();
            refreshMDStatus();
            refreshBollStatus();
            refreshTrdStatus();
            refreshPnlStatus();
        }
        void refreshTrdFlow(int volume,double price,int timeStamp)
        {
            if (m_pTradeFlow==NULL)
            {
                int idxCoef = 10000;
                int leaderID = m_pLegs.size() > 0? m_pLegs.at(0)->id(): -1;
                int laggerID = (m_pLegs.size() > 1? m_pLegs.at(1)->id(): -1) * idxCoef;
                for (int i=2; i<m_pLegs.size(); i++) { idxCoef *= 10; laggerID += m_pLegs.at(1)->id() * idxCoef; }
                m_pTradeFlow=m_pStrategy->createStrategyFlow(0, leaderID, laggerID);
            }
            m_pTradeFlow->FloatValue[0]=m_triggerPrice;
            m_pTradeFlow->IntValue[0]=m_triggerVolume;
            m_pTradeFlow->FloatValue[1]=price;
            m_pTradeFlow->IntValue[1]=volume;
            m_pTradeFlow->IntValue[2]=timeStamp;
            m_pStrategy->appendStrategyFlow(m_pTradeFlow);
        }
        int findDecPlc(double tic)
        {
            int decPlc;
            std::string ticStr = std::to_string(tic);
            decPlc = ticStr.find_last_not_of("0") - ticStr.find_last_of(".");
            g_pMercLog->log("findDecPlc,lst_not_0,%lu,lst_of_.,%lu,decPlc,%d", ticStr.find_last_not_of("0"), ticStr.find_last_of("."), decPlc);

            return decPlc;
        }
        bool inSession(int timeStamp)
        {
            for (auto pLeg: m_pLegs)
            {
                if (!pLeg->inSession(timeStamp))
                {
                    g_pMercLog->log("inSession,%s,%d,false", pLeg->ID(), timeStamp);
                    return false;
                }
            }
            return true;
        }

        void initComb(std::vector<CFutureExtentionAE *> &pLegs, std::vector<double> &coefs, std::vector<double> &exeCoefs)
        {
            m_pLegs = pLegs;
            m_coefs = coefs;
            m_exeCoefs = exeCoefs;
            m_pSpreadExec->m_pLegs = m_pLegs;
            m_pSpreadExec->m_coefs = m_coefs;
            m_pSpreadExec->m_exeCoefs = exeCoefs;

            m_sprdMulti = 0.0;
            for (int i=0; i<pLegs.size(); i++)
            {
                char coef[500];
                sprintf(coef, "%g", coefs.at(i));
                /* std::string coefStr = abs(coefs.at(i)) == 1? "": coef; */
                std::string coefStr = coef;
                m_sprdNm += coefStr + std::string(m_pLegs.at(i)->ID());
                if (i < pLegs.size() - 1)
                    m_sprdNm += m_pEnv->m_sprdConn;
                m_EDC2 += pLegs.at(i)->EDC();

                g_pMercLog->log("[initComb]legIdx,%d,legID,%s,coef,%s,sprdNmSofar,%s", i, m_pLegs.at(i)->ID(), coef, m_sprdNm.c_str());

				if (m_exeCoefs.at(i) != 0)
				{
					if (int(m_sprdMulti) == 0)
						m_sprdMulti = m_pLegs.at(i)->multiply();
					else
						m_sprdMulti = std::gcd(int(m_sprdMulti), int(m_pLegs.at(i)->multiply()));
                }
            }
            m_pSpreadExec->m_sprdMulti = m_sprdMulti;

            m_pSpreadExec->m_sprdNm = m_sprdNm;

            m_tick = m_pLegs.at(0)->tick();
            m_ticDecPlc = findDecPlc(m_tick);
            m_ticM = pow(10, m_ticDecPlc);
            m_gapThreshold = m_tick*VALID_GAP;

            m_multiply = 0.0;
            for (int i=0; i<pLegs.size(); i++)
            {
                if (m_exeCoefs.at(i) != 0)
                {
                    if (int(m_multiply) == 0)
                        m_multiply = pLegs.at(i)->multiply();
                    else
                        m_multiply = std::gcd(int(m_multiply), int(pLegs.at(i)->multiply()));
                }
            }
            g_pMercLog->log("[initComb]%s,coefsz,%lu,execoefsz,%lu,sprdMulti,%g,m_multiply,%g", m_sprdNm.c_str(), m_coefs.size(), m_exeCoefs.size(), m_sprdMulti, m_multiply);

            m_ltdc = getLstTrdDayCnt(m_pLegs.at(0)->m_pInstrument, m_pStrategy->getTradingDay(), true, m_pEnv->m_ltdD);

            m_marginPerPair = -DBL_MAX;
            if (m_pEnv->m_mrgnRt > 0.0)
            {
                std::string cmbNm = "";
                for (auto pLeg: m_pLegs)
                {
                    cmbNm += pLeg->ID();
                    double mrgn = pLeg->m_pMD->m_preClosePrice * pLeg->multiply() * m_pEnv->m_mrgnRt;
                    if (mrgn > m_marginPerPair)
                        m_marginPerPair = mrgn;
                    g_pMercLog->log("initComb,%s,mrgnrt,%g,mrgnperpair,%g,legCP,%g,tic,%g,ticDecPlc,%d,ticM,%g", cmbNm.c_str(), m_pEnv->m_mrgnRt, m_marginPerPair, pLeg->m_pMD->m_preClosePrice, m_tick, m_ticDecPlc, m_ticM);
                }
            }
            else
            {
                for (auto pLeg: m_pLegs)
                {
                    if (pLeg->marginPerLot() > m_marginPerPair)
                        m_marginPerPair = pLeg->marginPerLot();
                }
            }
        }

        void finishComb(CSpreadSignal *pSignal)
        {
            m_pSignal = pSignal;
            syncSig2Obj();
            adjMaxAmt();
            adjustPositionLimit(m_manSprdMaxLot);
            refreshPos();
            updateEdge();
            internalConstrain();
            g_pMercLog->log("[finishComb],%s,prdMaxAmt,%g,sprdMaxTradeSize,%d,sprdMaxLot,%d,stepSize,%d", m_sprdNm.c_str(), m_maxAmt, m_maxTradeSize, m_manSprdMaxLot, m_stepSize);
        }

        void syncSig2Obj()
        {
            m_refMid = m_pSignal->m_refMid;
            m_manSprdMaxLot = m_pSignal->m_sprdMaxLot;
            m_stepSize = m_pSignal->m_stepSize;
        }

        void adjMaxAmt()
        {
            m_maxAmt = m_manSprdMaxLot * m_marginPerPair;
        }
        void adjustPositionLimit(int positionLimit)
        {
            m_maxTradeSize = m_manSprdMaxLot;
            m_maxTradeSize = std::min(m_maxTradeSize, positionLimit);
            updtStepSize();
            internalConstrain();
        }
        void updtStepSize()
        {
            m_stepSize = std::min(m_stepSize, m_maxTradeSize);
            m_pSignal->m_stepSize = m_stepSize;
        }
       
        int refreshPos()
        {
            m_pos = m_pSignal->m_pos;
            return m_pos;
        }
        int getPos()
        {
            return m_pSignal->m_pos;
        }
        void internalConstrain()
        {
            if (abs(m_pos) > m_maxTradeSize)
            {
                m_internalConstrain = 1;
            }
            else
            {
                m_ttlMrgn = abs(m_pos) * m_marginPerPair;
                if (m_ttlMrgn > m_maxAmt)
                    m_internalConstrain = 1;
                else
                    m_internalConstrain = 0;
            }
            selfConstrain();
        }
        void preTradeConstrain()
        {
            if (hasStaticError())
            {
                addConstrain(4);
            }
            else if (m_ltdc < m_pEnv->m_minEDC)
            {
                addConstrain(3);
            }
            g_pMercLog->log("[preTradeConstrain],m_pSpreads,%s,ltdc,%d,minEDC,%d", m_sprdNm.c_str(), m_ltdc, m_pEnv->m_minEDC);
        }
        void addConstrain(int constrain)
        {
            m_externalConstrain=constrain;
            selfConstrain();
        }
        void selfConstrain()
        {
            m_selfConstrain = std::max(std::max(m_internalConstrain, m_externalConstrain), m_manTrdRt);
        }
        void updateEdge()
        {
            calcMean();
        } 

        double calcMean()
        {
            m_refMid = m_pSignal->m_refMid;
            return m_spAvg;
        }

        bool isReadyToTrade()
        {
            double GAP = 0.0;
            for (int i=0; i<m_pLegs.size(); i++)
            {
                if (!m_pLegs.at(i)->m_pMD->isReadyToTrade())
                    return false;
                GAP += m_pLegs.at(i)->GAP();
            }

            // TODO TODO TODO how to add back?
            /* if (GAP > m_gapThreshold) */
            /* { */
            /*     return false; */
            /* } */
            if (m_buy >= m_sell)
            {
                return false;
            }
            return true;
        }
        bool isSafeToBuy()
        {
            int sfTicCnt = 5;

            bool isSafeToBuy = true;
            for (int i=0; i<m_pLegs.size(); i++)
            {
                if (m_coefs.at(i) > 0)
                    isSafeToBuy = isSafeToBuy && m_pLegs.at(i)->m_pMD->isSafeToBuy(sfTicCnt);
                else
                    isSafeToBuy = isSafeToBuy && m_pLegs.at(i)->m_pMD->isSafeToSell(sfTicCnt);
            }
            return isSafeToBuy;
        }
        bool isSafeToSell()
        {
            int sfTicCnt = 5;

            bool isSafeToSell = true;
            for (int i=0; i<m_pLegs.size(); i++)
            {
                if (m_coefs.at(i) > 0)
                    isSafeToSell = isSafeToSell && m_pLegs.at(i)->m_pMD->isSafeToSell(sfTicCnt);
                else
                    isSafeToSell = isSafeToSell && m_pLegs.at(i)->m_pMD->isSafeToBuy(sfTicCnt);
            }
            return isSafeToSell;
        }
        void updatePrice(int timeStamp)
        {
            // This is for matching min data backtest; Ideally, update price can be done when not ready to trade
            if (!isReadyToTrade())
                return;

            m_spLAP = m_spAP;
            m_spLBP = m_spBP;
            m_spAQ = INT_MAX;
            m_spBQ = INT_MAX;
            m_spMP = 0.0;
            m_spAP = m_spBP = 0.0;
            m_exeSpAP = m_exeSpBP = 0.0;
            // Only update awp when in session
            bool inSsn = inSession(timeStamp);
            if (inSsn)
            {
                m_pSignal->m_awp = m_pSignal->m_sprdAP = m_pSignal->m_sprdBP = 0.0;
                m_pSignal->m_sprdAQ = m_pSignal->m_sprdBQ = INT_MAX;
            }
            for (int i=0; i<m_pLegs.size(); i++)
            {
                if (m_coefs.at(i) > 0)
                {
                    m_spAQ = std::min(m_spAQ, m_pLegs.at(i)->AQ());
                    m_spBQ = std::min(m_spBQ, m_pLegs.at(i)->BQ());
                    m_spAP += m_coefs.at(i) * m_pLegs.at(i)->AP();
                    m_spBP += m_coefs.at(i) * m_pLegs.at(i)->BP();
                    if (inSsn)
                    {
                        m_pSignal->m_sprdAQ = std::min(m_pSignal->m_sprdAQ, m_pLegs.at(i)->AQ());
                        m_pSignal->m_sprdBQ = std::min(m_pSignal->m_sprdBQ, m_pLegs.at(i)->BQ());
                        m_pSignal->m_sprdAP += m_coefs.at(i) * m_pLegs.at(i)->AP();
                        m_pSignal->m_sprdBP += m_coefs.at(i) * m_pLegs.at(i)->BP();
                    }
                }
                else
                {
                    m_spAQ = std::min(m_spAQ, m_pLegs.at(i)->BQ());
                    m_spBQ = std::min(m_spBQ, m_pLegs.at(i)->AQ());
                    m_spAP += m_coefs.at(i) * m_pLegs.at(i)->BP();
                    m_spBP += m_coefs.at(i) * m_pLegs.at(i)->AP();
                    if (inSsn)
                    {
                        m_pSignal->m_sprdAQ = std::min(m_pSignal->m_sprdAQ, m_pLegs.at(i)->BQ());
                        m_pSignal->m_sprdBQ = std::min(m_pSignal->m_sprdBQ, m_pLegs.at(i)->AQ());
                        m_pSignal->m_sprdAP += m_coefs.at(i) * m_pLegs.at(i)->BP();
                        m_pSignal->m_sprdBP += m_coefs.at(i) * m_pLegs.at(i)->AP();
                    }
                }
                m_spMP += m_coefs.at(i) * m_pLegs.at(i)->m_pMD->defaultPrice();

                double legMultiCoef = m_pLegs.at(i)->multiply() / m_sprdMulti;
                if (m_exeCoefs.at(i) > 0)
                {
                    m_exeSpAP += m_exeCoefs.at(i) * m_pLegs.at(i)->AP() * legMultiCoef;
                    m_exeSpBP += m_exeCoefs.at(i) * m_pLegs.at(i)->BP() * legMultiCoef;
                }
                else
                {
                    m_exeSpAP += m_exeCoefs.at(i) * m_pLegs.at(i)->BP() * legMultiCoef;
                    m_exeSpBP += m_exeCoefs.at(i) * m_pLegs.at(i)->AP() * legMultiCoef;
                }
                if (inSsn)
                {
                    m_pSignal->m_awp += m_exeCoefs.at(i) * m_pLegs.at(i)->LP() * legMultiCoef;
                }
            }

            if (m_pEnv->m_isBacktest && m_lastTrdVlm!=0)
            {
                vlmDecayLogic();
            }
        }
        void vlmDecayLogic()
        {
            if (m_lastTrdPrice >= m_spAP && m_spAP == m_spLAP)
            {
                m_spAQ = std::max(0, int(m_spAQ - m_lastTrdVlm));
            }
            else if (m_lastTrdPrice <= m_spBP && m_spBP == m_spLBP)
            {
                m_spBQ = std::max(0, int(m_spBQ - m_lastTrdVlm));
            }
            else
            {
                m_lastTrdVlm = 0;
            }
        }
        int trySignal(int constrain,int timeStamp, bool &toSyncData)
        {
            updatePrice(timeStamp);
            constrain = std::max(m_selfConstrain,constrain);

            if (constrain>0 && m_pSignal->m_pos==0) return 0;
            
            // Check risk boundary breaks (only when not constrained)
            if (constrain == 0)
            {
                double currentSpread = m_spAvg;
                checkRiskBoundaryBreak(currentSpread, timeStamp);
            }
            
            int act = 0;
            switch (constrain)
            {
            case 0:
                act = updateSignal(toSyncData, false);
                break;
            case 1:
                act = updateSignal(toSyncData, true);
                break;
            case 2:
                act = squeezeSignal(timeStamp);
                break;
            case 3:
                act = clearSignal(timeStamp);
                break;
            default:
                break;
            }

            if (m_pEnv->m_isBacktest > 0 && m_pEnv->m_cancelRate>0)
            {
                if (constrain < 2 && act != 0)
                {
                    int a = rand()%100;
                    if (a<int(m_pEnv->m_cancelRate*100))
                    {
                        return 0;
                    }
                }
            }
            if (act > 0 && isSafeToBuy()) return act;
            else if (act < 0 && isSafeToSell()) return act;

            refreshPrMdlOpnStatus();
            refreshPrMdlClsStatus();
            refreshPrMdlParamStatus();
            refreshPrMdlParamTicStatus();
            refreshEMAStatus();
            refreshSprdIdxStatus();
            refreshMDStatus();
            refreshBollStatus();
            return 0;
        }
        
        int sign(double val)
        {
            return (val > 0) ? 1 : ((val < 0) ? -1 : 0);
        }
        
        std::string identifyLosingLeg(double spreadMom, const std::vector<double>& legPricesAtStart, const std::vector<double>& currentLegPrices)
        {
            if (m_pLegs.size() < 2 || legPricesAtStart.size() < 2 || currentLegPrices.size() < 2)
            {
                return "";
            }
            
            // Calculate momentum for each leg
            double baseMom = currentLegPrices[0] - legPricesAtStart[0];
            double hedgeMom = currentLegPrices[1] - legPricesAtStart[1];
            
            int baseSign = baseMom > 0 ? 1 : (baseMom < 0 ? -1 : 0);
            int hedgeSign = hedgeMom > 0 ? 1 : (hedgeMom < 0 ? -1 : 0);
            int spreadSign = spreadMom > 0 ? 1 : (spreadMom < 0 ? -1 : 0);
            
            std::string losingLeg = "";
            
            // Logic from Python to identify losing leg
            if (baseSign == hedgeSign)
            {
                if (spreadSign == baseSign)
                {
                    losingLeg = "base";
                }
                else
                {
                    losingLeg = "hedge";
                }
            }
            else
            {
                if (std::abs(baseMom) >= std::abs(hedgeMom))
                {
                    if (spreadSign == baseSign)
                    {
                        losingLeg = "base";
                    }
                    else
                    {
                        losingLeg = "hedge";
                    }
                }
                else
                {
                    if (spreadSign == baseSign)
                    {
                        losingLeg = "hedge";
                    }
                    else
                    {
                        losingLeg = "base";
                    }
                }
            }
            
            g_pMercLog->log("[identifyLosingLeg],%s,baseMom,%g,hedgeMom,%g,spreadMom,%g,baseSign,%d,hedgeSign,%d,spreadSign,%d,losingLeg,%s",
                m_sprdNm.c_str(), baseMom, hedgeMom, spreadMom, baseSign, hedgeSign, spreadSign, losingLeg.c_str());
            
            return losingLeg;
        }
        
        void checkRiskBoundaryBreak(double currentSpread, int timeStamp)
        {
            // Check if we need to enter risk mode
            if (!m_pSignal->m_inRiskMode)
            {
                double center = (m_arbitrageLower + m_arbitrageUpper) / 2.0;
                
                if (currentSpread > m_riskUpper)
                {
                    // Upper boundary break
                    m_pSignal->m_inRiskMode = true;
                    m_pSignal->m_breakDirection = 1;
                    m_pSignal->m_maxSpread = currentSpread;
                    m_pSignal->m_centerAtRiskStart = center;
                    
                    // Store current leg prices
                    m_pSignal->m_legPricesAtRiskStart.clear();
                    for (auto pLeg : m_pLegs)
                    {
                        m_pSignal->m_legPricesAtRiskStart.push_back(pLeg->LP());
                    }
                    
                    g_pMercLog->log("[checkRiskBoundaryBreak],%s,UPPER_BREAK,spread,%g,riskUpper,%g,center,%g",
                        m_sprdNm.c_str(), currentSpread, m_riskUpper, center);
                    
                    // Trigger position reduction
                    reduceLosingLegPosition(currentSpread, timeStamp);
                }
                else if (currentSpread < m_riskLower)
                {
                    // Lower boundary break
                    m_pSignal->m_inRiskMode = true;
                    m_pSignal->m_breakDirection = -1;
                    m_pSignal->m_minSpread = currentSpread;
                    m_pSignal->m_centerAtRiskStart = center;
                    
                    // Store current leg prices
                    m_pSignal->m_legPricesAtRiskStart.clear();
                    for (auto pLeg : m_pLegs)
                    {
                        m_pSignal->m_legPricesAtRiskStart.push_back(pLeg->LP());
                    }
                    
                    g_pMercLog->log("[checkRiskBoundaryBreak],%s,LOWER_BREAK,spread,%g,riskLower,%g,center,%g",
                        m_sprdNm.c_str(), currentSpread, m_riskLower, center);
                    
                    // Trigger position reduction
                    reduceLosingLegPosition(currentSpread, timeStamp);
                }
            }
            else
            {
                // Already in risk mode - check for rebound
                checkReboundAndRestore(currentSpread, timeStamp);
            }
        }
        
        void reduceLosingLegPosition(double currentSpread, int timeStamp)
        {
            if (m_pLegs.size() < 2 || m_pSignal->m_legPricesAtRiskStart.empty())
            {
                return;
            }
            
            // Calculate momentum
            double spreadMom = currentSpread - m_pSignal->m_centerAtRiskStart;
            
            std::vector<double> currentLegPrices;
            for (auto pLeg : m_pLegs)
            {
                currentLegPrices.push_back(pLeg->LP());
            }
            
            // Identify losing leg
            std::string losingLeg = identifyLosingLeg(spreadMom, m_pSignal->m_legPricesAtRiskStart, currentLegPrices);
            
            if (losingLeg.empty())
            {
                return;
            }
            
            m_pSignal->m_reducedLeg = losingLeg;
            
            // Calculate how much to reduce
            int legIndex = (losingLeg == "base") ? 0 : 1;
            CFutureExtentionAE* pLeg = m_pLegs[legIndex];
            
            // Get current position of this leg
            int legPos = pLeg->pos();
            double absLegPos = std::abs(static_cast<double>(legPos));
            
            m_pSignal->m_maxVirtualAbs = absLegPos;
            
            double amountToReduce = m_reduceRatio * absLegPos;
            
            // Round to step size if needed
            if (m_stepSize > 0)
            {
                amountToReduce = std::floor(amountToReduce / m_stepSize) * m_stepSize;
            }
            
            if (amountToReduce > 0 && legPos != 0)
            {
                int reduceDirection = -sign(static_cast<double>(legPos));
                m_pSignal->m_reducedDirection = reduceDirection;
                m_pSignal->m_reducedAmount = amountToReduce;
                
                // Note: Actual order placement would happen through the execution system
                // This is tracked separately from the arbitrage position
                m_pSignal->m_riskPos += reduceDirection * static_cast<int>(amountToReduce);
                
                g_pMercLog->log("[reduceLosingLegPosition],%s,losingLeg,%s,legPos,%d,amountToReduce,%g,reduceDirection,%d,riskPos,%d",
                    m_sprdNm.c_str(), losingLeg.c_str(), legPos, amountToReduce, reduceDirection, m_pSignal->m_riskPos);
            }
        }
        
        void checkReboundAndRestore(double currentSpread, int timeStamp)
        {
            if (m_pSignal->m_reducedLeg.empty())
            {
                return;
            }
            
            bool shouldRestore = false;
            double reboundAmount = m_exitInterval; // Use exit interval as rebound threshold
            
            if (m_pSignal->m_breakDirection == 1)
            {
                // Upper break - check for downward rebound
                if (currentSpread > m_pSignal->m_maxSpread)
                {
                    m_pSignal->m_maxSpread = currentSpread;
                }
                
                if (currentSpread <= m_pSignal->m_maxSpread - reboundAmount)
                {
                    shouldRestore = true;
                    g_pMercLog->log("[checkReboundAndRestore],%s,REBOUND_DOWN,currentSpread,%g,maxSpread,%g,reboundAmount,%g",
                        m_sprdNm.c_str(), currentSpread, m_pSignal->m_maxSpread, reboundAmount);
                }
            }
            else if (m_pSignal->m_breakDirection == -1)
            {
                // Lower break - check for upward rebound
                if (currentSpread < m_pSignal->m_minSpread)
                {
                    m_pSignal->m_minSpread = currentSpread;
                }
                
                if (currentSpread >= m_pSignal->m_minSpread + reboundAmount)
                {
                    shouldRestore = true;
                    g_pMercLog->log("[checkReboundAndRestore],%s,REBOUND_UP,currentSpread,%g,minSpread,%g,reboundAmount,%g",
                        m_sprdNm.c_str(), currentSpread, m_pSignal->m_minSpread, reboundAmount);
                }
            }
            
            // Also check if all positions are closed
            if (m_pSignal->m_openPositions.empty())
            {
                shouldRestore = true;
                g_pMercLog->log("[checkReboundAndRestore],%s,ALL_POSITIONS_CLOSED", m_sprdNm.c_str());
            }
            
            if (shouldRestore)
            {
                // Restore the reduced position
                int signedSupplement = -m_pSignal->m_reducedDirection * static_cast<int>(m_pSignal->m_reducedAmount);
                
                // Note: Actual order placement would happen through the execution system
                m_pSignal->m_riskPos += signedSupplement;
                
                g_pMercLog->log("[checkReboundAndRestore],%s,RESTORE,leg,%s,supplement,%d,riskPos,%d",
                    m_sprdNm.c_str(), m_pSignal->m_reducedLeg.c_str(), signedSupplement, m_pSignal->m_riskPos);
                
                // Reset risk mode
                m_pSignal->m_inRiskMode = false;
                m_pSignal->m_reducedLeg = "";
                m_pSignal->m_reducedAmount = 0.0;
                m_pSignal->m_reducedDirection = 0;
                m_pSignal->m_breakDirection = 0;
            }
        }

        int updateSignal(bool &toSyncData, bool closeOnly=false)
        {
            int trdSz=0;
            double buyBfr = m_buy;
            double sellBfr = m_sell;
            updtBuySell(m_buy, m_sell);

            updtPnl();
            if (buyBfr != m_buy || sellBfr != m_sell)
            {
                toSyncData = true;
            }

            if (!isReadyToTrade())
            {
                return trdSz;
            }
           
            int pos = m_pSignal->m_pos;

            int currBchPos = abs(pos);
            if (m_stepSize > 0)
            {
                currBchPos = pos > 0? abs(pos) % m_stepSize: -abs(pos) % m_stepSize;
                if (currBchPos == 0)
                    currBchPos = std::min(m_stepSize, abs(pos));
            }

            if (m_spBP >= m_sell && isSafeToSell())
            {
                int trdSzMax = INT_MAX;
                if (pos <= 0)
                    trdSzMax = std::max(m_maxTradeSize + pos, 0);
                else
                    trdSzMax = pos;
                
                trdSz = std::min(std::max(m_stepSize, 1), trdSzMax);

                if (pos <= 0)
                    trdSz = std::min(trdSz, trdSzMax);
                else if (pos > 0)
                    trdSz = std::min(trdSz, abs(currBchPos));

                if (closeOnly && pos > 0)
                    trdSz = std::min(std::min(m_stepSize, trdSzMax), abs(currBchPos));

                // when short, trdSz needs to be negative
                trdSz = -trdSz;
            }
            else if (m_spAP <= m_buy && isSafeToBuy())
            {
                int trdSzMax = INT_MAX;
                if (pos >= 0)
                    trdSzMax = std::max(m_maxTradeSize - pos, 0);
                else
                    trdSzMax = -pos;
                
                trdSz = std::min(std::max(m_stepSize, 1), trdSzMax);

                if (pos >= 0)
                    trdSz = std::min(trdSz, trdSzMax);
                else if (pos < 0)
                    trdSz = std::min(trdSz, abs(currBchPos));

                if (closeOnly && pos < 0)
                    trdSz = std::min(std::min(m_stepSize, trdSzMax), abs(currBchPos));
            }
            return trdSz;
        }
        
        void updateDailyHighLow(int tradingDay, double currentSpread)
        {
            // Update current day tracking
            if (m_pSignal->m_currentDay != tradingDay)
            {
                // New day - save previous day's high/low
                if (m_pSignal->m_currentDay > 0)
                {
                    CDailyHighLow dayData(m_pSignal->m_currentDay, 
                                         m_pSignal->m_currentDayHigh, 
                                         m_pSignal->m_currentDayLow);
                    m_pSignal->m_dailyHighLows.push_back(dayData);
                    
                    // Keep only max history days
                    while (m_pSignal->m_dailyHighLows.size() > static_cast<size_t>(m_pSignal->m_maxHistoryDays))
                    {
                        m_pSignal->m_dailyHighLows.pop_front();
                    }
                }
                
                // Reset for new day
                m_pSignal->m_currentDay = tradingDay;
                m_pSignal->m_currentDayHigh = currentSpread;
                m_pSignal->m_currentDayLow = currentSpread;
            }
            else
            {
                // Update current day's high/low
                if (currentSpread > m_pSignal->m_currentDayHigh)
                {
                    m_pSignal->m_currentDayHigh = currentSpread;
                }
                if (currentSpread < m_pSignal->m_currentDayLow)
                {
                    m_pSignal->m_currentDayLow = currentSpread;
                }
            }
        }
        
        void calculateBoundaries(int tradingDay, double currentSpread)
        {
            // Update daily high/low first
            updateDailyHighLow(tradingDay, currentSpread);
            
            // Calculate arbitrage boundaries from last N days
            double arbLower = DBL_MAX;
            double arbUpper = -DBL_MAX;
            
            // Include current day
            if (m_pSignal->m_currentDayHigh > arbUpper)
            {
                arbUpper = m_pSignal->m_currentDayHigh;
            }
            if (m_pSignal->m_currentDayLow < arbLower)
            {
                arbLower = m_pSignal->m_currentDayLow;
            }
            
            // Look back arbitrageN days
            int lookbackDays = 0;
            for (auto it = m_pSignal->m_dailyHighLows.rbegin(); 
                 it != m_pSignal->m_dailyHighLows.rend() && lookbackDays < m_arbitrageN; 
                 ++it, ++lookbackDays)
            {
                if (it->dailyHigh > arbUpper)
                {
                    arbUpper = it->dailyHigh;
                }
                if (it->dailyLow < arbLower)
                {
                    arbLower = it->dailyLow;
                }
            }
            
            // Calculate risk boundaries from last riskN days
            double riskLower = DBL_MAX;
            double riskUpper = -DBL_MAX;
            
            // Include current day
            if (m_pSignal->m_currentDayHigh > riskUpper)
            {
                riskUpper = m_pSignal->m_currentDayHigh;
            }
            if (m_pSignal->m_currentDayLow < riskLower)
            {
                riskLower = m_pSignal->m_currentDayLow;
            }
            
            // Look back riskN days
            lookbackDays = 0;
            for (auto it = m_pSignal->m_dailyHighLows.rbegin(); 
                 it != m_pSignal->m_dailyHighLows.rend() && lookbackDays < m_riskN; 
                 ++it, ++lookbackDays)
            {
                if (it->dailyHigh > riskUpper)
                {
                    riskUpper = it->dailyHigh;
                }
                if (it->dailyLow < riskLower)
                {
                    riskLower = it->dailyLow;
                }
            }
            
            // Update boundaries
            m_arbitrageLower = arbLower;
            m_arbitrageUpper = arbUpper;
            m_riskLower = riskLower;
            m_riskUpper = riskUpper;
            
            g_pMercLog->log("[calculateBoundaries],%s,day,%d,arbLower,%g,arbUpper,%g,riskLower,%g,riskUpper,%g",
                m_sprdNm.c_str(), tradingDay, arbLower, arbUpper, riskLower, riskUpper);
        }

        void updateBoundariesAndGrids(double newArbitrageLower, double newArbitrageUpper, double newRiskLower, double newRiskUpper)
        {
            // Check if boundaries have changed
            bool boundariesChanged = (newArbitrageLower != m_pSignal->m_prevArbitrageLower || 
                                     newArbitrageUpper != m_pSignal->m_prevArbitrageUpper);
            
            if (!boundariesChanged)
            {
                return;
            }
            
            g_pMercLog->log("[updateBoundariesAndGrids],%s,ArbLower,%g->%g,ArbUpper,%g->%g",
                m_sprdNm.c_str(), m_pSignal->m_prevArbitrageLower, newArbitrageLower,
                m_pSignal->m_prevArbitrageUpper, newArbitrageUpper);
            
            // Update boundary values
            m_arbitrageLower = newArbitrageLower;
            m_arbitrageUpper = newArbitrageUpper;
            m_riskLower = newRiskLower;
            m_riskUpper = newRiskUpper;
            
            double boundDistance = m_arbitrageUpper - m_arbitrageLower;
            if (boundDistance <= 0)
            {
                return;
            }
            
            // Adjust dynamic factors based on profitable rates
            double profitableRateLong = 0.0;
            double profitableRateShort = 0.0;
            
            if (m_pSignal->m_numOpensLong > 0)
            {
                profitableRateLong = static_cast<double>(m_pSignal->m_profitableClosesLong) / m_pSignal->m_numOpensLong;
            }
            
            if (m_pSignal->m_numOpensShort > 0)
            {
                profitableRateShort = static_cast<double>(m_pSignal->m_profitableClosesShort) / m_pSignal->m_numOpensShort;
            }
            
            // Adjust long dynamic factor
            if (m_pSignal->m_numOpensLong >= m_minOps)
            {
                if (profitableRateLong < m_widenThreshold)
                {
                    // Low profitable rate - widen grid to reduce frequency
                    m_pSignal->m_dynamicFactorLong *= m_widenStep;
                }
                else if (profitableRateLong > m_narrowThreshold)
                {
                    // High profitable rate - narrow grid to increase frequency
                    m_pSignal->m_dynamicFactorLong /= m_widenStep;
                }
                // Clamp to min/max range
                m_pSignal->m_dynamicFactorLong = std::max(m_minDynamic, std::min(m_maxDynamic, m_pSignal->m_dynamicFactorLong));
            }
            
            // Adjust short dynamic factor
            if (m_pSignal->m_numOpensShort >= m_minOps)
            {
                if (profitableRateShort < m_widenThreshold)
                {
                    m_pSignal->m_dynamicFactorShort *= m_widenStep;
                }
                else if (profitableRateShort > m_narrowThreshold)
                {
                    m_pSignal->m_dynamicFactorShort /= m_widenStep;
                }
                m_pSignal->m_dynamicFactorShort = std::max(m_minDynamic, std::min(m_maxDynamic, m_pSignal->m_dynamicFactorShort));
            }
            
            g_pMercLog->log("[updateBoundariesAndGrids],%s,ProfitRateLong,%g,ProfitRateShort,%g,DynFactorLong,%g,DynFactorShort,%g",
                m_sprdNm.c_str(), profitableRateLong, profitableRateShort,
                m_pSignal->m_dynamicFactorLong, m_pSignal->m_dynamicFactorShort);
            
            // Recalculate grids with new factors
            double equityPerSet = 0.0;
            for (int i = 0; i < m_pLegs.size(); i++)
            {
                equityPerSet += std::abs(m_coefs[i]) * m_pLegs[i]->LP();
            }
            
            int currentMaxSets = m_manSprdMaxLot;
            if (equityPerSet > 0 && m_maxLeverage > 0)
            {
                double currentCash = 100000.0; // TODO: Get from account
                int leverageMaxSets = static_cast<int>(currentCash * m_maxLeverage / equityPerSet);
                if (currentMaxSets == 0 || leverageMaxSets < currentMaxSets)
                {
                    currentMaxSets = leverageMaxSets;
                }
            }
            if (currentMaxSets <= 0) currentMaxSets = 1;
            
            double entryInterval = (boundDistance / 2.0) / currentMaxSets;
            if (entryInterval < m_minEntryInterval)
            {
                entryInterval = m_minEntryInterval;
            }
            
            double entryIntervalLong = entryInterval * m_pSignal->m_dynamicFactorLong;
            double entryIntervalShort = entryInterval * m_pSignal->m_dynamicFactorShort;
            double center = (m_arbitrageLower + m_arbitrageUpper) / 2.0;
            double centerLong = center - m_exitInterval / 2.0;
            double centerShort = center + m_exitInterval / 2.0;
            
            // Store old grids for position adjustment
            std::vector<double> oldLongGrids = m_pSignal->m_longGrids;
            std::vector<double> oldShortGrids = m_pSignal->m_shortGrids;
            
            // Generate new grids
            m_pSignal->m_longGrids.clear();
            m_pSignal->m_shortGrids.clear();
            
            for (int k = 1; k <= m_maxGridLevels; k++)
            {
                m_pSignal->m_longGrids.push_back(centerLong - k * entryIntervalLong);
                m_pSignal->m_shortGrids.push_back(centerShort + k * entryIntervalShort);
            }
            
            // Adjust existing open positions to new grid levels
            // TODO: Implement position adjustment when we add position tracking
            
            // Update previous boundary tracking
            m_pSignal->m_prevArbitrageLower = newArbitrageLower;
            m_pSignal->m_prevArbitrageUpper = newArbitrageUpper;
            
            // Store intervals for reference
            m_pSignal->m_entryIntervalLong = entryIntervalLong;
            m_pSignal->m_entryIntervalShort = entryIntervalShort;
        }

        void updtBuySell(double &buy, double &sell)
        {
            // Default values if grid not configured
            buy = -DBL_MAX; 
            sell = DBL_MAX;
            
            // Skip if grid parameters not configured
            if (m_exitInterval <= 0.0 || m_arbitrageLower >= m_arbitrageUpper)
            {
                return;
            }
            
            // Calculate base entry interval
            double boundDistance = m_arbitrageUpper - m_arbitrageLower;
            double currentCash = 100000.0; // TODO: Get from account or config
            double equityPerSet = 0.0;
            
            for (int i = 0; i < m_pLegs.size(); i++)
            {
                equityPerSet += std::abs(m_coefs[i]) * m_pLegs[i]->LP();
            }
            
            int currentMaxSets = m_manSprdMaxLot;
            if (equityPerSet > 0 && m_maxLeverage > 0)
            {
                int leverageMaxSets = static_cast<int>(currentCash * m_maxLeverage / equityPerSet);
                if (currentMaxSets == 0 || leverageMaxSets < currentMaxSets)
                {
                    currentMaxSets = leverageMaxSets;
                }
            }
            if (currentMaxSets <= 0) currentMaxSets = 1;
            
            // Calculate entry interval
            double entryInterval = (boundDistance / 2.0) / currentMaxSets;
            if (entryInterval < m_minEntryInterval)
            {
                entryInterval = m_minEntryInterval;
            }
            
            // Apply dynamic factors
            double entryIntervalLong = entryInterval * m_pSignal->m_dynamicFactorLong;
            double entryIntervalShort = entryInterval * m_pSignal->m_dynamicFactorShort;
            
            // Calculate centers
            double center = (m_arbitrageLower + m_arbitrageUpper) / 2.0;
            double centerLong = center - m_exitInterval / 2.0;
            double centerShort = center + m_exitInterval / 2.0;
            
            // Determine buy/sell based on position
            int pos = m_pSignal->m_pos;
            
            if (pos == 0)
            {
                // No position - use center-based pricing
                buy = centerLong;
                sell = centerShort;
            }
            else if (pos > 0)
            {
                // Long position - calculate next level for adding or closing
                // For closing: current position + exit_interval
                sell = centerLong + std::abs(pos) * entryIntervalLong / m_stepSize + m_exitInterval;
                
                // For adding: next grid level down
                buy = centerLong - (std::abs(pos) / m_stepSize + 1) * entryIntervalLong;
            }
            else // pos < 0
            {
                // Short position - calculate next level for adding or closing
                // For closing: current position - exit_interval
                buy = centerShort - std::abs(pos) * entryIntervalShort / m_stepSize - m_exitInterval;
                
                // For adding: next grid level up
                sell = centerShort + (std::abs(pos) / m_stepSize + 1) * entryIntervalShort;
            }
            
            // Store calculated intervals for tracking
            m_pSignal->m_entryIntervalLong = entryIntervalLong;
            m_pSignal->m_entryIntervalShort = entryIntervalShort;
            
            return;
        }

        int squeezeSignal(int timeStamp)
        {
            int pos = m_pSignal->m_pos;
            int a = int(timeStamp*0.001/m_pEnv->m_twapSecond);
            int b = int(m_snapSecond*1.0/m_pEnv->m_twapSecond);
            int adjPosDiff = abs(pos) - m_maxTradeSize;
            int act = 0;
            if (a != b && adjPosDiff > 0)
            {
                m_snapSecond = int(timeStamp*0.001);
                int adjPos = std::min(adjPosDiff,m_stepSize);
                if (pos>0)
                {
                    act = -std::min(adjPos,m_spBQ);
                }
                else
                {
                    act = std::min(adjPos,m_spAQ);
                }
            }
            return act;
        }
        int clearSignal(int timeStamp)
        {
            int pos = m_pSignal->m_pos;
            int a = int(timeStamp*0.001/m_pEnv->m_twapSecond);
            int b = int(m_snapSecond*1.0/m_pEnv->m_twapSecond);
            int act = 0;
            if (a != b && pos != 0)
            {
                m_snapSecond = int(timeStamp*0.001);
                if (pos > 0)
                {
                    act = -std::min(pos,m_spBQ);
                }
                else
                {
                    act = std::min(-pos,m_spAQ);
                }
            }
            return act;
        }
        const char *legID(int i) { return m_pLegs.at(i)->ID(); }
        bool hasStaticError()
        {
            bool hasErr = false;
            for (auto pLeg: m_pLegs)
            {
                hasErr = hasErr || pLeg->staticError();
            }
            return hasErr;
        }
        bool needRefreshMD(int id)
        {
            if (m_coefs.size() < 2)
                return true;

            int fastLeg = m_pLegs.at(0)->m_pInstrument->getInstrumentRef();
            int slowLeg = m_pLegs.at(1)->m_pInstrument->getInstrumentRef();
            if (m_pFuzzySorter->isFaster(fastLeg,slowLeg) < 0)
            {
                fastLeg = m_pLegs.at(1)->m_pInstrument->getInstrumentRef();
                slowLeg = m_pLegs.at(0)->m_pInstrument->getInstrumentRef();
            }
            if (id==fastLeg)
            {
                m_isPreFlush=true;
                return false;
            }
            else if (id==slowLeg || (m_pFuzzySorter->isFaster(slowLeg,id)>0 && m_isPreFlush))
            {
                m_isPreFlush=false;
                return true;
            }
            return false;
        }
        // TODO: is this reasonable?
        int chooseLeg(int action)
        {
            int tryLegID = 0;
            double bidQSum = 0.0;
            double askQSum = 0.0;
            for (auto pLeg: m_pLegs)
            {
                bidQSum += pLeg->BQ();
                askQSum += pLeg->AQ();
            }
            double maxAff = -DBL_MAX;
            for (int i=0; i<m_pLegs.size(); i++)
            {
                double legAff = action*m_coefs.at(i) > 0 ?  bidQSum / m_pLegs.at(i)->AQ(): askQSum / m_pLegs.at(i)->BQ();
                if (m_exeCoefs.at(i) == 0)
                    legAff = -DBL_MAX;

                if (legAff > maxAff)
                {
                    maxAff = legAff;
                    tryLegID = i;
                }
            }
            return tryLegID;
        }
        void notifyExecStarted(int action)
        {
            m_triggerVolume = action;
            m_triggerPrice = action > 0 ? m_exeSpAP : m_exeSpBP;
            m_pSpreadExec->m_sprdTgtPr = action > 0? m_buy: m_sell;
        }

        void notifyExecFinished(int spreadTrdVolume,double spreadTrdPrice,int timeStamp, double spreadExePrice)
        {
            int prevPos = m_pSignal->m_pos;
            
            notifyOpenTrade(spreadTrdVolume,spreadTrdPrice, spreadExePrice);
            m_pSignal->m_pos += spreadTrdVolume;
            
            // Track opens and closes for dynamic grid adjustment
            bool isOpening = (prevPos == 0 || (prevPos > 0 && spreadTrdVolume > 0) || (prevPos < 0 && spreadTrdVolume < 0));
            bool isClosing = (prevPos > 0 && spreadTrdVolume < 0) || (prevPos < 0 && spreadTrdVolume > 0);
            
            if (isOpening)
            {
                // Store leg prices at entry for risk management
                std::vector<double> legPricesAtEntry;
                for (auto pLeg : m_pLegs)
                {
                    legPricesAtEntry.push_back(pLeg->LP());
                }
                
                // Add to open positions list with leg prices
                int direction = spreadTrdVolume > 0 ? 1 : -1;
                COpenPosition newPos(spreadTrdPrice, direction, spreadExePrice, legPricesAtEntry);
                m_pSignal->m_openPositions.push_back(newPos);
                
                if (spreadTrdVolume > 0)
                {
                    m_pSignal->m_numOpensLong++;
                }
                else if (spreadTrdVolume < 0)
                {
                    m_pSignal->m_numOpensShort++;
                }
                
                g_pMercLog->log("[notifyExecFinished],%s,OPEN,vol,%d,entryPrice,%g,spread,%g,legPricesCount,%lu",
                    m_sprdNm.c_str(), spreadTrdVolume, spreadTrdPrice, spreadExePrice, legPricesAtEntry.size());
            }
            
            if (isClosing)
            {
                // Calculate if this was profitable and remove from open positions
                double pnl = 0.0;
                if (prevPos > 0 && spreadTrdVolume < 0)
                {
                    // Closing long
                    pnl = (spreadExePrice - m_pSignal->m_atp) * std::abs(spreadTrdVolume);
                    m_pSignal->m_numClosesLong++;
                    if (pnl > 0)
                    {
                        m_pSignal->m_profitableClosesLong++;
                    }
                    
                    // Remove oldest long position
                    for (auto it = m_pSignal->m_openPositions.begin(); it != m_pSignal->m_openPositions.end(); ++it)
                    {
                        if (it->direction == 1)
                        {
                            m_pSignal->m_openPositions.erase(it);
                            break;
                        }
                    }
                }
                else if (prevPos < 0 && spreadTrdVolume > 0)
                {
                    // Closing short
                    pnl = (m_pSignal->m_atp - spreadExePrice) * std::abs(spreadTrdVolume);
                    m_pSignal->m_numClosesShort++;
                    if (pnl > 0)
                    {
                        m_pSignal->m_profitableClosesShort++;
                    }
                    
                    // Remove oldest short position
                    for (auto it = m_pSignal->m_openPositions.begin(); it != m_pSignal->m_openPositions.end(); ++it)
                    {
                        if (it->direction == -1)
                        {
                            m_pSignal->m_openPositions.erase(it);
                            break;
                        }
                    }
                }
                
                g_pMercLog->log("[notifyExecFinished],%s,CLOSE,vol,%d,pnl,%g,openPosCount,%lu",
                    m_sprdNm.c_str(), spreadTrdVolume, pnl, m_pSignal->m_openPositions.size());
            }
            
            refreshPos();

            internalConstrain();
            refreshBollStatus();
            refreshTrdStatus();
            refreshPnlStatus();
            refreshTrdFlow(spreadTrdVolume,spreadExePrice,timeStamp);
        }
        void notifyOpenTrade(int volume, double price, double exePr)
        {
            if (m_pSignal->m_pos + volume == 0)
            {
                m_pSignal->m_dnPnl += (exePr - m_pSignal->m_atp) * (-volume) * m_multiply;
                m_pSignal->m_diPnl = 0;
                m_pSignal->m_pnl = m_pSignal->m_diPnl + m_pSignal->m_dnPnl;
                m_pSignal->m_atp = 0;
            }
            else
                m_pSignal->m_atp = (m_pSignal->m_atp * m_pSignal->m_pos + exePr * volume) / (m_pSignal->m_pos + volume);

            m_lastTrdPrice = exePr;
            m_lastTrdVlm = abs(volume);
        }
        void settle() { updtPnl(); }
        void updtPnl()
        {
            double lp = m_pSignal->m_awp;
            m_pSignal->m_diPnl = (lp - m_pSignal->m_atp) * m_pSignal->m_pos * m_multiply;
            m_pSignal->m_pnl = m_pSignal->m_diPnl + m_pSignal->m_dnPnl;
        }
        void logDetail() { g_pMercLog->log("%d,%s,signal %d,selfConstrain %d", m_id, m_sprdNm.c_str(), m_pos, m_selfConstrain); }
    };
    CStratsEnvAE m_env;
    CAccountManager *m_pAccountManager;
    CFuzzySort *m_pFuzzySorter;
    CTradeControl *m_pTradeControl;
    CSignalManagerAE *m_pSignalManager;
    CSpreadSignalManager *m_pSpreadManager;
    CForceTaskManager *m_pForceTaskManager;
    const volatile int *m_pCurTimeStamp;
    
    std::map<std::string, unsigned> m_nameMap;
    std::map<int, CFutureExtentionAE* > m_pFutures;
    std::map<int, CSpreadExtentionAE* > m_pSpreads;
    std::map<int, CSpreadExtentionAE* > m_pTrdSprds;
    std::map<int, const CMercStrategyOrderItem* > m_orderMap;
    std::map<std::string, int> m_sprdNmPosMap;
    
    bool m_strategyReady;
    bool m_needOnBar;

    double m_totalMargin;
    int m_sendCount;
    int m_failedCount;
    int m_cancelCount;
    int m_tradeCount;
    int m_sendVolume;
    int m_cancelVolume;
    int m_tradeVolume;

    int m_mdTS;

    int m_triggerStart;
    std::map<int, int> m_instTriggerMap;
    std::map<int, int> m_sortedSpreads;

public:        
    char m_buffer[100];
    CLogFile *m_pTrdLog;
    CLogFile *m_pTrdFlw;

    CMercStrategyStatus *m_pRiskStatus0;
    CMercStrategyStatus *m_pRiskStatus1;
    virtual bool initStrategy(void)
    {
        useHistoryPosition();
        
        m_env.init(this);
        m_env.refreshParameterStatus();
        createAccountManager();
        m_env.m_dataFn = m_env.m_shmNmPrefix + std::string(m_env.m_strategyName) + ".json";
        m_pFuzzySorter=new CFuzzySort(m_env.m_needFuzzySort);
        m_pSignalManager=new CSignalManagerAE(m_env.m_dataFn.c_str());
        m_pSpreadManager=new CSpreadSignalManager(m_env.m_dataFn.c_str(), &m_env);
        m_pForceTaskManager=new CForceTaskManager(0,&m_env,m_env.m_maxWorker);
        m_strategyReady=m_needOnBar=false;
        m_totalMargin=0.0;
        m_triggerStart = 0;
        m_sendCount=m_failedCount=m_cancelCount=m_tradeCount=m_sendVolume=m_cancelVolume=m_tradeVolume=0;
        m_pRiskStatus0=m_pRiskStatus1=NULL;
        g_pMercLog->log("initStrategy,done");
        return true;
    }
    void createAccountManager()
    {
        if (strcmp(m_env.m_accountID,"None")==0)
        {
            m_pAccountManager = getAccountManagerByPos(0);
        }
        else
        {
            m_pAccountManager = getAccountManager(m_env.m_accountID);
        }
        if (m_pAccountManager==NULL)
        {
            g_pMercLog->log("%s,exit: no account,accountID %s",m_env.m_strategyName,m_env.m_accountID);
            exit(1);
        }
    }
    virtual void strategyReady(void)
    {
        createFile();
        startSubscribe();
        createSpreads();
        controlPositionLimit();
        m_pCurTimeStamp = getCurTimeStampPtr();
        m_pTradeControl = new CTradeControl(this,&m_env);
        m_pTradeControl->init();
        updateInstTriggerMap();
        updateBiasSlf();
        updateConstrain();
        refreshRiskStatus();
        if (m_env.m_isBacktest>0 && m_env.m_cancelRate>0)
        {
            srand(time(0));
        }
        m_strategyReady=true;
        g_pMercLog->log("strategyReady,done");
    }
    void updateInstTriggerMap()
    {
        int *sortedFutures = m_pFuzzySorter->sortByScore();
        int idx = 0;
        for (int i=0;i<m_pFuzzySorter->size();i++)
        {
            int ref = sortedFutures[i];
            for (auto& it : m_pTrdSprds)
            {
                int slwLeg = it.second->m_pLegs.at(0)->m_pInstrument->getInstrumentRef();
                for (auto pLeg: it.second->m_pLegs)
                {
                    int legRef = pLeg->m_pInstrument->getInstrumentRef();
                    if (m_pFuzzySorter->isFaster(legRef, slwLeg)<0)
                        slwLeg = legRef;
                }

                if (ref == slwLeg)
                {
                    m_sortedSpreads[idx] = it.first;
                    idx++;
                }
            }
            m_instTriggerMap[ref] = idx;
        }
    }
    void refreshRiskStatus()
    {
        if (m_pRiskStatus0==NULL)
        {
            m_pRiskStatus0=createStrategyStatus(4);
        }
        m_pRiskStatus0->IntValue[0]=m_pTradeControl->getTradeConstrain();
        m_pRiskStatus0->IntValue[1]=0;
        m_pRiskStatus0->IntValue[2]=0;
        m_pRiskStatus0->IntValue[3]=0;
        m_pRiskStatus0->FloatValue[0]=m_totalMargin;
        refreshStrategyStatus(m_pRiskStatus0);
        if (m_pRiskStatus1==NULL)
        {
            m_pRiskStatus1=createStrategyStatus(5);
        }
        m_pRiskStatus1->IntValue[0]=m_sendVolume;
        m_pRiskStatus1->IntValue[1]=m_cancelVolume;
        m_pRiskStatus1->IntValue[2]=m_tradeVolume;
        m_pRiskStatus1->IntValue[3]=m_failedCount;
        m_pRiskStatus1->FloatValue[0]=(m_sendCount>0) ? double(100*m_cancelCount/m_sendCount) : 0.0;
        m_pRiskStatus1->FloatValue[1]=(m_sendVolume>0) ? double(100*m_cancelVolume/m_sendVolume) : 0.0;
        refreshStrategyStatus(m_pRiskStatus1);
    }
    void createFile(void)
    {
        // Trade log
        char trdLogDir[500];
        sprintf(trdLogDir, "trdLog_%s_%s", m_env.m_strategyName, m_env.m_productName);
        int ok = 0;
        if ((ok=mkdir(trdLogDir, 0755)) != 0)
        {
            g_pMercLog->log("[createFile]%s ok: %d", trdLogDir, ok);
        }
        m_pTrdLog = makeLogFile(trdLogDir);

        // Trade flow
        char trdFlwDir[500];
        sprintf(trdFlwDir, "trdFlw_%s_%s", m_env.m_strategyName, m_env.m_productName);
        ok = 0;
        if ((ok=mkdir(trdFlwDir, 0755)) != 0)
        {
            g_pMercLog->log("[createFile]%s ok: %d", trdFlwDir, ok);
        }
        m_pTrdFlw = makeLogFile(trdFlwDir);
    }

    CSpreadExtentionAE *getSprdArb(std::string sprdNm)
    {
        for (auto pSprd: m_pSpreads)
        {
            if (sprdNm == getSprdNm(pSprd.second))
                return pSprd.second;
        }
        return nullptr;
    }

    CSpreadExtentionAE *getSprd(std::string sprdNm)
    {
        for (auto pSprd: m_pSpreads)
        {
            if (sprdNm == getSprdNm(pSprd.second))
                return pSprd.second;
        }
        return nullptr;
    }

    void updtSprdNmPosMap()
    {
        m_sprdNmPosMap.clear();
        for (auto pSprd: m_pTrdSprds)
        {
            m_sprdNmPosMap[pSprd.second->m_sprdNm] = pSprd.second->getPos();
        }
        for (auto pSprd: m_pTrdSprds)
        {
            pSprd.second->m_sprdNmPos = m_sprdNmPosMap[pSprd.second->m_sprdNm];
        }
    }

    void syncData(bool prt=false)
    {
        json outJ = json::parse(R"(
        {
            "ois": {},
            "lps": {},
            "awps": {},
            "sprd_aps": {},
            "sprd_bps": {},
            "sprd_aqs": {},
            "sprd_bqs": {},
            "trd_sprds": [],
            "sprds": [],
            "step_sizes": {},
            "theo_bids": {},
            "theo_asks": {},
            "sprd_max_lots": {},
            "ref_mids": {},
            "inst_poss": {},
            "sprd_poss": {},
            "mrgns": {},
            "atps": {},
            "di_pnls": {},
            "dn_pnls": {},
            "pnls": {},
        }
        )");

        for (auto& pInst: m_pFutures)
        {
            std::string instKey = std::string(pInst.second->ID()) + "." + pInst.second->exchangeID();

            outJ["ois"][instKey] = pInst.second->preOI();
            outJ["lps"][instKey] = pInst.second->LP();
            outJ["awps"][instKey] = pInst.second->m_pSignal->m_theoLst;
            outJ["inst_poss"][instKey] = pInst.second->m_pSignal->m_pos;
        }

        std::vector<std::string> trdSprds;
        std::vector<std::string> spreads;
        for (auto& it: m_pSpreads)
        {
            spreads.push_back(it.second->m_sprdNm);
        }
        for (auto& it: m_pTrdSprds)
        {
            trdSprds.push_back(it.second->m_sprdNm);
        }

        for (auto& pSprd: m_pSpreads)
        {
            std::string sprdNm = pSprd.second->m_sprdNm;

            outJ["sprd_poss"][sprdNm] = pSprd.second->m_pSignal->m_pos;
            outJ["mrgns"][sprdNm] = pSprd.second->m_ttlMrgn;
            outJ["ref_mids"][sprdNm] = pSprd.second->m_pSignal->m_refMid;
            outJ["sprd_max_lots"][sprdNm] = pSprd.second->m_pSignal->m_sprdMaxLot;
            outJ["step_sizes"][sprdNm] = pSprd.second->m_pSignal->m_stepSize;
            outJ["awps"][sprdNm] = pSprd.second->m_pSignal->m_awp;
            outJ["sprd_aps"][sprdNm] = pSprd.second->m_pSignal->m_sprdAP;
            outJ["sprd_bps"][sprdNm] = pSprd.second->m_pSignal->m_sprdBP;
            outJ["sprd_aqs"][sprdNm] = pSprd.second->m_pSignal->m_sprdAQ;
            outJ["sprd_bqs"][sprdNm] = pSprd.second->m_pSignal->m_sprdBQ;
            outJ["theo_bids"][sprdNm] = pSprd.second->m_buy;
            outJ["theo_asks"][sprdNm] = pSprd.second->m_sell;

            for (auto& pSprd1: m_pSpreads)
            {
                if (pSprd.second->m_coefs.size() != 2 || pSprd1.second->m_coefs.size() != 2) continue;
                if (pSprd.second->m_sprdNm == pSprd1.second->m_sprdNm) continue;
            }

            outJ["atps"][sprdNm] = pSprd.second->m_pSignal->m_atp;
            outJ["di_pnls"][sprdNm] = pSprd.second->m_pSignal->m_diPnl;
            outJ["dn_pnls"][sprdNm] = pSprd.second->m_pSignal->m_dnPnl;
            outJ["pnls"][sprdNm] = pSprd.second->m_pSignal->m_pnl;
            
            // Persist dynamic grid state
            outJ["dynamic_factor_long"][sprdNm] = pSprd.second->m_pSignal->m_dynamicFactorLong;
            outJ["dynamic_factor_short"][sprdNm] = pSprd.second->m_pSignal->m_dynamicFactorShort;
            outJ["num_opens_long"][sprdNm] = pSprd.second->m_pSignal->m_numOpensLong;
            outJ["num_opens_short"][sprdNm] = pSprd.second->m_pSignal->m_numOpensShort;
            outJ["profitable_closes_long"][sprdNm] = pSprd.second->m_pSignal->m_profitableClosesLong;
            outJ["profitable_closes_short"][sprdNm] = pSprd.second->m_pSignal->m_profitableClosesShort;
            
            // Persist risk management state
            outJ["in_risk_mode"][sprdNm] = pSprd.second->m_pSignal->m_inRiskMode;
            outJ["reduced_leg"][sprdNm] = pSprd.second->m_pSignal->m_reducedLeg;
            outJ["reduced_amount"][sprdNm] = pSprd.second->m_pSignal->m_reducedAmount;
            outJ["reduced_direction"][sprdNm] = pSprd.second->m_pSignal->m_reducedDirection;
            outJ["arbitrage_pos"][sprdNm] = pSprd.second->m_pSignal->m_arbitragePos;
            outJ["risk_pos"][sprdNm] = pSprd.second->m_pSignal->m_riskPos;
            
            // Persist daily high/low history
            json dailyHighLowsArray = json::array();
            for (const auto& dayData : pSprd.second->m_pSignal->m_dailyHighLows)
            {
                json dayObj;
                dayObj["day"] = dayData.tradingDay;
                dayObj["high"] = dayData.dailyHigh;
                dayObj["low"] = dayData.dailyLow;
                dailyHighLowsArray.push_back(dayObj);
            }
            outJ["daily_high_lows"][sprdNm] = dailyHighLowsArray;
            outJ["current_day"][sprdNm] = pSprd.second->m_pSignal->m_currentDay;
            outJ["current_day_high"][sprdNm] = pSprd.second->m_pSignal->m_currentDayHigh;
            outJ["current_day_low"][sprdNm] = pSprd.second->m_pSignal->m_currentDayLow;
        }
        outJ["trd_sprds"] = trdSprds;
        outJ["sprds"] = spreads;

        std::ofstream o(m_env.m_dataFn);
        o << std::setw(4) << outJ << std::endl;
        if (prt)
            g_pMercLog->log("[SYNC_DATA]%s", outJ.dump().c_str());
    }

    void loadInsts(std::vector<const CInstrument *> &instruments)
    {
        std::vector<std::string> instStrs;
        for (auto sprdInsts: m_env.m_manSprdInsts)
        {
            std::vector<std::string> insts = sprdInsts.second;
            g_pMercLog->log("[loadInsts] sprdInsts.size,%lu", insts.size());
            for (auto inst: insts)
            {
                g_pMercLog->log("[loadInsts] loading,%s", inst.c_str());
                if (find(instStrs.begin(), instStrs.end(), inst) == instStrs.end())
                {
                    instStrs.push_back(inst);
                }
            }

        }
        for (auto instStr: instStrs)
        {
            g_pMercLog->log("[loadInsts]instStrs,%s", instStr.c_str());
            const CInstrument * inst = getInstrument(instStr.c_str());
            if (inst != nullptr)
                instruments.push_back(inst);
        }

    }
    void loadInstsOld(std::vector<const CInstrument *> &instruments)
    {
        for (auto prdNm: m_env.m_prdNms)
        {
            std::vector<const CInstrument *> prdInsts;
            getInstrumentsByProduct(prdNm.c_str(), prdInsts);
            instruments.insert(instruments.end(), prdInsts.begin(), prdInsts.end());
        }
    }

    void startSubscribe()
    {
        std::vector<const CInstrument *> instruments;
        loadInstsOld(instruments);

        const CProduct* pProd = nullptr;
        for (auto prdNm: m_env.m_prdNms)
        {
            pProd = getProduct(prdNm.c_str());
            if (pProd==NULL) { g_pMercLog->log("%s|exit: prod %s does't exist",m_env.m_strategyName, prdNm.c_str()); exit(1); }
        }
        if (instruments.size() < 1) { g_pMercLog->log("%s,exit: no INSTS to subscribe for prod %s",m_env.m_strategyName,m_env.m_productName); exit(1); }

        // read mdqp CSVFile:
        std::map<std::string,int> mdqp_map;

        const char* exchg = pProd->getExchangeID();
        CCSVFile *pCoefFile=NULL;
        if (strcmp(exchg,"SHFE")==0)
        {
            std::string defaultFileName = m_env.m_mdqpFolder;
            defaultFileName = defaultFileName+"shfe_mdqp.csv";
            pCoefFile=readCSV(defaultFileName.c_str());
            g_pMercLog->log("%s|try open %s",m_env.m_strategyName,defaultFileName.c_str());
            if (pCoefFile==NULL)
            {
                std::string defaultFileName2 = m_env.m_mdqpFolder;
                defaultFileName2 = defaultFileName2+"shfe_mdqp_"+std::to_string(getTradingDay())+".csv";
                pCoefFile=readCSV(defaultFileName2.c_str());
                g_pMercLog->log("%s|try open %s",m_env.m_strategyName,defaultFileName2.c_str());
            }
        }
        else if (strcmp(exchg,"INE")==0)
        {
            std::string defaultFileName = m_env.m_mdqpFolder;
            defaultFileName = defaultFileName+"ine_mdqp.csv";
            pCoefFile=readCSV(defaultFileName.c_str());
            g_pMercLog->log("%s|try open %s",m_env.m_strategyName,defaultFileName.c_str());
            if (pCoefFile==NULL)
            {
                std::string defaultFileName2 = m_env.m_mdqpFolder;
                defaultFileName2 = defaultFileName2+"ine_mdqp_"+std::to_string(getTradingDay())+".csv";
                pCoefFile=readCSV(defaultFileName2.c_str());
                g_pMercLog->log("%s|try open %s",m_env.m_strategyName,defaultFileName2.c_str());
            }
        }
        if (pCoefFile!=NULL)
        {
            while (pCoefFile->getLine())
            {
                const char *InstrumentNo = pCoefFile->getFieldByName("InstrumentNo");
                const char *InstrumentID=pCoefFile->getFieldByName("InstrumentID");
                if (InstrumentNo!=NULL && InstrumentID!=NULL)
                {
                    int value = atoi(InstrumentNo);
                    mdqp_map[InstrumentID] = value;
                }
            }
            pCoefFile->destroy();
        }
        // sort instruments by mdqp:
        for (unsigned i=0;i<instruments.size();i++)
        {
            const CInstrument *pInst = instruments[i];
            unsigned k = i;
            for (unsigned j=i+1;j<instruments.size();j++)
            {
                const CInstrument *pInst2 = instruments[j];
                const char* instrumentID2 = pInst2->getInstrumentID();
                int mdqp_j = (mdqp_map.find(instrumentID2)!=mdqp_map.end()) ? mdqp_map[instrumentID2] : j;

                const CInstrument *pInst3 = instruments[k];
                const char* instrumentID3 = pInst3->getInstrumentID();
                int mdqp_k = (mdqp_map.find(instrumentID3)!=mdqp_map.end()) ? mdqp_map[instrumentID3] : k;
                if (mdqp_j < mdqp_k)
                {
                    k = j;
                }
            }
            instruments[i] = instruments[k];
            instruments[k] = pInst;
        }

        for (unsigned i=0;i<instruments.size();i++)
        {
            const CInstrument *pInst = instruments[i];
            const char* instrumentID = pInst->getInstrumentID();
            int instRef = pInst->getInstrumentRef();
            if (m_pFutures.find(instRef) != m_pFutures.end())
            {
                g_pMercLog->log("%s|exit: duplicate INSTS to handle|INST %s",m_env.m_strategyName,instrumentID);
                exit(1);
            }
            if (!subscribe(instrumentID,instRef))
            {
                g_pMercLog->log("%s|exit: subscribe failed|INST %s",m_env.m_strategyName,instrumentID);
                exit(1);
            }
            if (!m_pFuzzySorter->subscribeInst(instRef))
            {
                g_pMercLog->log("%s|exit: add sorter failed|INST %s",m_env.m_strategyName,instrumentID);
                exit(1);
            }
            CSignalAE *pSignal = m_pSignalManager->getSignal(instrumentID);
            if (pSignal == NULL)
            {
                g_pMercLog->log("%s|exit: resume signal failed|INST %s",m_env.m_strategyName,instrumentID);
                exit(1);
            }
            else
            {
                m_nameMap[instrumentID] = instRef;
                CFutureExtentionAE *pFuture = new CFutureExtentionAE(instRef,this,&m_env,pInst,pSignal);
                pFuture->checkStaticError();
                m_pFutures[instRef] = pFuture;
                /* int futMonth = dt2Mth(pFuture->m_pMD->m_expirationDate); */
                /* g_pMercLog->log("%s,startSubscribe,INST,%s,futMonth,%d,preOI,%d",m_env.m_strategyName,instrumentID, futMonth, pFuture->preOI()); */
            }
        }
        m_pSignalManager->freeSignal(m_nameMap);
        g_pMercLog->log("%s,finish subscribe,%d,%s,prod %s,insts %d",
                        m_env.m_strategyName, m_env.m_pStrategy->getTradingDay(), getTimeString(m_buffer, m_env.m_pStrategy->getCurTimeStamp(), true),m_env.m_productName,int(m_pFutures.size()));

    }
    void createSpreads()
    {
        g_pMercLog->log("[createSpreads],manSprds,sz,%lu", m_env.m_manSprds.size());
        if (!m_env.m_manSprds.empty())
        {
            createSpreadsByManSprds();
        }
        for (auto& it : m_pSpreads)
        {
            it.second->preTradeConstrain();
            if (it.second->m_pos!=0 || it.second->m_selfConstrain==0)
            {
                m_pTrdSprds[it.first] = it.second;
                it.second->refreshAllStatus();
                it.second->logDetail();
            }
        }
        for (auto& it : m_pSpreads)
            g_pMercLog->log("[createSpreads],m_pSpreads,%s,pos,%d,slfConstrain,%d", it.second->m_sprdNm.c_str(), it.second->m_pos, it.second->m_selfConstrain);
        for (auto& it : m_pTrdSprds)
            g_pMercLog->log("[createSpreads],m_pTrdSprds,%s", it.second->m_sprdNm.c_str());

        freeSpreadSignal();
        g_pMercLog->log("%s,created spreads,count %d,tradable %d",m_env.m_strategyName,int(m_pSpreads.size()),int(m_pTrdSprds.size()));

        updateBiasSlf();

        bool toSyncData = false;
        for (auto& it : m_pTrdSprds)
        {
            m_pCurTimeStamp = getCurTimeStampPtr();
            it.second->trySignal(0, *m_pCurTimeStamp, toSyncData);
        }
        if (toSyncData)
            syncData();
    }

    void createSpreadsByManSprds()
    {
        std::vector<std::string> manSprds(m_env.m_manSprds);
        std::vector<std::string> xstSprds;

        int count = m_pSpreadManager->size();
        for (int i=0;i<count;i++)
        {
            g_pMercLog->log("%s,[createSpreadsByManSprds],getSignal,%d,outof,%d", m_env.m_strategyName, i, count);
            CSpreadSignal *pSignal=m_pSpreadManager->getSignal(i);
            if (pSignal!=NULL)
            {
                auto sprdNm = pSignal->m_sprdNm;
                xstSprds.push_back(sprdNm);
                g_pMercLog->log("%s,[xstSprds],push_back,%s", m_env.m_strategyName, sprdNm.c_str());

                if (find(m_env.m_manSprds.begin(), m_env.m_manSprds.end(), sprdNm) == m_env.m_manSprds.end())
                {
                    manSprds.push_back(sprdNm);
                }
            }
        }
        for (auto manSprdNm: manSprds)
        {
            g_pMercLog->log("%s,[createSpreadsByManSprds],creating,%s", m_env.m_strategyName, manSprdNm.c_str());
            std::vector<CFutureExtentionAE *> pLegs;
            std::vector<std::string> instNms;
            std::vector<double> coefs;
            std::vector<double> exeCoefs = m_env.m_manSprdExeCoefs[manSprdNm];
            m_pSpreadManager->parseSprdNm(manSprdNm, instNms, coefs);
            for (int i=0; i<instNms.size(); i++)
            {
                if (exeCoefs.size() > 0)
                    g_pMercLog->log("%s,[createSpreadsByManSprds],parsed,into,%s,coef,%g,exeCoef,%g", m_env.m_strategyName, instNms[i].c_str(), coefs[i], exeCoefs[i]);
                else
                    g_pMercLog->log("%s,[createSpreadsByManSprds],parsed,into,%s,coef,%g,NO exeCoef,0,AS_NOT_IN_CFG", m_env.m_strategyName, instNms[i].c_str(), coefs[i]);
            }

            bool genSprdOK = true;
            for (int i=0; i<instNms.size(); i++)
            {
                const CInstrument *pLeg = getInstrument(instNms.at(i).c_str());
                if (pLeg == nullptr)
                {
                    g_pMercLog->log("%s,[createSpreadsByManSprds],leg,%s,isNULL", m_env.m_strategyName, instNms.at(i).c_str());
                    genSprdOK = false;
                    break;
                }
                if (m_pFutures.find(pLeg->getInstrumentRef()) == m_pFutures.end())
                {
                    g_pMercLog->log("%s,[createSpreadsByManSprds],leg,%s,ref,%d,NOT_FOUND", m_env.m_strategyName, instNms.at(i).c_str(), pLeg->getInstrumentRef());
                    genSprdOK = false;
                    break;
                }
                CFutureExtentionAE * leg = m_pFutures[pLeg->getInstrumentRef()];
                pLegs.push_back(leg);
            }
            if (!genSprdOK)
            {
                g_pMercLog->log("%s,[createSpreadsByManSprds],genSprdOK,%d", m_env.m_strategyName, genSprdOK);
                continue;
            }

            int id = m_pSpreads.size();
            CSpreadExtentionAE *pSpread = new CSpreadExtentionAE(id, this, &m_env, m_pFuzzySorter);
            pSpread->initComb(pLegs, coefs, exeCoefs);

            bool inXst = find(xstSprds.begin(), xstSprds.end(), manSprdNm) != xstSprds.end();
            bool inMan = find(m_env.m_manSprds.begin(), m_env.m_manSprds.end(), manSprdNm) != m_env.m_manSprds.end();
            if (inXst && !inMan)
            {
                pSpread->addConstrain(1);
            }

            CSpreadSignal *pSignal = getSprdSignal(pSpread);
            if (pSignal==NULL)
            {
                g_pMercLog->log("resume spread signal failed");
                exit(1);
            }
            else if (inXst && !inMan)
            {
                if (pSignal->m_pos != 0)
                {
                    g_pMercLog->log("NOT ZERO POSITION SPREAD MUST BE IN CFG, EXIT NOW...,manSprdNm,%s,pos,%d,inXsg,!inMan", manSprdNm.c_str(), pSignal->m_pos);
                    exit(1);
                }
            }

            pSpread->m_pSignal = pSignal;

            // fillin manual spread config
            if (m_env.m_manTrdRts.find(manSprdNm) != m_env.m_manTrdRts.end())
                pSpread->m_manTrdRt = m_env.m_manTrdRts[manSprdNm];

            pSpread->m_refMid = pSpread->m_pSignal->m_refMid;
            if (m_env.m_manRefMids.find(manSprdNm) != m_env.m_manRefMids.end())
            {
                double refMid = m_env.m_manRefMids[manSprdNm];
                if (refMid < DBL_MAX)
                    pSpread->m_refMid = pSpread->m_pSignal->m_refMid = refMid;
            }

            if (m_env.m_manSprdMaxLots.find(manSprdNm) != m_env.m_manSprdMaxLots.end())
            {
                int maxLot = m_env.m_manSprdMaxLots[manSprdNm];
                if (maxLot > 0)
                {
                    pSpread->m_manSprdMaxLot = pSpread->m_pSignal->m_sprdMaxLot = maxLot;
                }
            }

            if (m_env.m_manSprdStpLots.find(manSprdNm) != m_env.m_manSprdStpLots.end())
            {
                int stpLot = m_env.m_manSprdStpLots[manSprdNm];
                if (stpLot > 0)
                {
                    pSpread->m_stepSize = pSpread->m_pSignal->m_stepSize = stpLot;
                }
            }
            
            // Load dynamic grid strategy parameters from XML (if provided)
            // These parameters can be specified globally in the Strategy XML node
            const CXMLNode *pStrategyDesc = getStrategyDesc();
            if (pStrategyDesc != nullptr)
            {
                pSpread->m_exitInterval = pStrategyDesc->getDoubleProperty("GridExitInterval", 0.0);
                pSpread->m_minEntryInterval = pStrategyDesc->getDoubleProperty("MinEntryInterval", 0.0);
                pSpread->m_minDynamic = pStrategyDesc->getDoubleProperty("MinDynamicFactor", 1.0);
                pSpread->m_maxDynamic = pStrategyDesc->getDoubleProperty("MaxDynamicFactor", 3.0);
                pSpread->m_widenThreshold = pStrategyDesc->getDoubleProperty("WidenThreshold", 0.3);
                pSpread->m_narrowThreshold = pStrategyDesc->getDoubleProperty("NarrowThreshold", 0.7);
                pSpread->m_widenStep = pStrategyDesc->getDoubleProperty("WidenStep", 1.2);
                pSpread->m_minOps = pStrategyDesc->getIntProperty("MinOpsForAdjust", 10);
                pSpread->m_reduceRatio = pStrategyDesc->getDoubleProperty("ReduceRatio", 0.6);
                pSpread->m_maxLeverage = pStrategyDesc->getDoubleProperty("MaxLeverage", 20.0);
                pSpread->m_maxGridLevels = pStrategyDesc->getIntProperty("MaxGridLevels", 500);
                
                // Boundary calculation window sizes
                pSpread->m_arbitrageN = pStrategyDesc->getIntProperty("ArbitrageN", 120);
                pSpread->m_riskN = pStrategyDesc->getIntProperty("RiskN", 180);
                pSpread->m_updateIntervalMinutes = pStrategyDesc->getIntProperty("UpdateIntervalMinutes", 15);
                
                // Set max history days to keep
                int maxHistoryDays = std::max(pSpread->m_arbitrageN, pSpread->m_riskN);
                pSpread->m_pSignal->m_maxHistoryDays = maxHistoryDays + 10; // Keep a bit extra
                
                // Set initial boundary values (these will be updated dynamically)
                pSpread->m_arbitrageLower = pStrategyDesc->getDoubleProperty("InitArbitrageLower", 0.0);
                pSpread->m_arbitrageUpper = pStrategyDesc->getDoubleProperty("InitArbitrageUpper", 0.0);
                pSpread->m_riskLower = pStrategyDesc->getDoubleProperty("InitRiskLower", 0.0);
                pSpread->m_riskUpper = pStrategyDesc->getDoubleProperty("InitRiskUpper", 0.0);
                
                // Initialize signal's boundary tracking
                if (pSpread->m_arbitrageLower > 0.0 || pSpread->m_arbitrageUpper > 0.0)
                {
                    pSpread->m_pSignal->m_prevArbitrageLower = pSpread->m_arbitrageLower;
                    pSpread->m_pSignal->m_prevArbitrageUpper = pSpread->m_arbitrageUpper;
                }
                
                g_pMercLog->log("[createSpreadsByManSprds],GridParams,sprd,%s,exitInterval,%g,minEntry,%g,minDyn,%g,maxDyn,%g,widen,%g,narrow,%g,reduceRatio,%g,maxLeverage,%g,arbN,%d,riskN,%d",
                    manSprdNm.c_str(), pSpread->m_exitInterval, pSpread->m_minEntryInterval, 
                    pSpread->m_minDynamic, pSpread->m_maxDynamic, pSpread->m_widenThreshold, 
                    pSpread->m_narrowThreshold, pSpread->m_reduceRatio, pSpread->m_maxLeverage,
                    pSpread->m_arbitrageN, pSpread->m_riskN);
            }

            pSpread->finishComb(pSignal);

            m_pSpreads[id] = pSpread;
        }
    }
    void freeSpreadSignal()
    {
        int count = m_pSpreadManager->size();
        for (int i=0;i<count;i++)
        {
            CSpreadSignal *pSignal=m_pSpreadManager->getSignal(i);
            if (pSignal!=NULL && notInclude(pSignal))
            {
                if (pSignal->m_pos==0) { m_pSpreadManager->freeSignal(pSignal); }
            }
        }
    }
    void controlPositionLimit()
    {
        int maxPrdLot = 0;
        for (auto& it : m_pTrdSprds)
        {
            maxPrdLot += it.second->m_manSprdMaxLot;
            it.second->adjustPositionLimit(it.second->m_manSprdMaxLot);
        }

        for (auto prdNm: m_env.m_prdNms)
            setProductPositionLimit(prdNm.c_str(), maxPrdLot, maxPrdLot);

        int minEDC = 1e6;
        for (auto& it : m_pTrdSprds)
        {
            if (it.second->m_EDC <= minEDC)
            {
                minEDC = it.second->m_EDC;
            }
        }
    }

    std::string getSprdNm(CSpreadExtentionAE *pSprd)
    {
        return pSprd->m_sprdNm;
    }

    CSpreadSignal *getSprdSignal(CSpreadExtentionAE *pSpread, bool createNew=true)
    {
        return m_pSpreadManager->getSignal(pSpread->m_sprdNm);
    }

    bool notInclude(CSpreadSignal *pSig)
    {
        for (auto& it : m_pSpreads)
        {
            if (it.second->m_sprdNm == pSig->m_sprdNm) return false; 
        }
        return true;
    }

    void updateBiasSlf()
    {
    }
    void updateConstrain()
    {
        m_totalMargin=0.0;
        for (auto& it : m_pFutures)
        {
            m_totalMargin += it.second->margin();
        }
        m_totalMargin = m_totalMargin*0.5;

        int constrain=0;
        if (m_failedCount >= 10)
        {
            constrain=4;
        }
        else if (getNetAvailable(m_pAccountManager) <= m_env.m_minAvailable)
        {
            constrain=1;
        }
        m_pTradeControl->addConstrain(constrain);
    }
    virtual void notifyMarketData(const CMarketData *pMarketData,int tag)
    {
        if (m_pTradeControl->m_onDayEnd || !m_strategyReady)
        {
            return;
        }
        int constrain = m_pTradeControl->getTradeConstrain();
        CFutureExtentionAE *pFuture = m_pFutures[tag];
        bool toSyncData = false;
        if (constrain < 4)
        {
            pFuture->updatePrice(pMarketData);
            triggerForceOrder(pFuture);

            int ts = *m_pCurTimeStamp;
            m_mdTS = pMarketData->getUpdateTimeStamp();
            bool isNewSnap = m_pFuzzySorter->updateOne(tag,ts,m_mdTS)>0;
            bool isSafeTS = (pFuture->inSession(m_mdTS) && pFuture->inSession(m_mdTS+15000));
            toSyncData = triggerSpread(tag,ts,constrain,isNewSnap,isSafeTS);

            if (toSyncData)
                syncData();
        }
        else if (m_pTradeControl->m_onDaySettle && !pFuture->hasOrder())
        {
            dailySettle(tag);
        }
        m_needOnBar=true;
    }
    int triggerSpread(int tag,int ts,int constrain,bool newSnap,bool safeTS)
    {
        bool toSyncData = false;

        if (newSnap)
        {
            m_triggerStart = 0;
        }
        int triggerEnd = m_instTriggerMap[tag];
        for (int i=m_triggerStart;i<triggerEnd;i++)
        {
            CSpreadExtentionAE *pSpread =  m_pTrdSprds[m_sortedSpreads[i]];
            CSpreadExec *pExec = pSpread->m_pSpreadExec;
            if (!pExec->isProcessing() && safeTS)
            {
                int action = pSpread->trySignal(constrain, ts, toSyncData);
                if (action != 0)
                {
                    int tryLegID = m_env.m_tryLegID > -1? m_env.m_tryLegID: pSpread->chooseLeg(action);
                    pSpread->notifyExecStarted(action);
                    pExec->start(action, tryLegID);
                    sendTryOrder(pExec);

                    syncData();
                }
            }
        }
        m_triggerStart = triggerEnd;

        return toSyncData;
    }
    void triggerForceOrder(CFutureExtentionAE *pFuture)
    {
        for (auto& it : pFuture->m_pForceTasks)
        {
            CForceTask *pTask = m_pForceTaskManager->get(it.first);
            if (pTask!=NULL && pTask->hasTask() && pTask->m_pLeg->m_pInstrument->getInstrumentRef()==pFuture->m_pInstrument->getInstrumentRef())
            {
                pTask->notifyMD();
                if (pTask->needResendOrder())
                {
                    CSpreadExec *pExec = m_pTrdSprds[pTask->spreadID()]->m_pSpreadExec;
                    if(!sendForceOrder(pExec, pTask))
                    {
                        return;
                    }
                }
            }
        }
    }
    const CMercStrategyOrderItem *sendOrder(const CInstrument *pInstrument,int type,int direction,double price,int volume, int reason)
    {
        CInputOrder inputOrder;
        inputOrder.setOrderType(type);
        inputOrder.setDirection(direction);
        inputOrder.setPrice(price);
        inputOrder.setOrderVolume(volume);
        const CMercStrategyOrderItem *pOrderItem=controledInsertOrder(&inputOrder,pInstrument,m_pAccountManager,m_env.m_offsetStrategy);
        if (pOrderItem != NULL)
        {
            int orderID = inputOrder.getOrderRef();
            pOrderItem->m_userInt1 = orderID;
            pOrderItem->m_userLongLong1 = 0;
            m_orderMap[orderID] = pOrderItem;
#if ODR_REASON
            // reason: 0-tryorder 1-forceorder 2-others
            g_pMercLog->log("SENDORDER_SUCCEED,TradingDay,%d,MarketDataTimeStamp,%d,InstrumentID,%s,reason,%d,volume,%d,price,%g,direction,%d,type,%d,orderid,%d,errorno,%d", getTradingDay(), m_mdTS, pInstrument->getInstrumentID(), reason, volume, price, direction, type, orderID, getLastErrorNo());
#endif
            return pOrderItem;
        }
        else
        {
            g_pMercLog->log("orderSendFailed|%d_%s,[%s],%d@%g,direction,%d,type,%d", getTradingDay(), getTimeString(m_buffer, getCurTimeStamp(), true), pInstrument->getInstrumentID(), volume, price, direction, type);
            return NULL;
        }
    }
    void sendTryOrder(CSpreadExec *pExec)
    {
        pExec->prepareTryOrder();
        int type = pExec->m_tryOrderType;
        int direction = pExec->m_tryOrderDirection;
        double price = pExec->m_tryOrderPrice;
        int volume = pExec->m_tryOrderVolume;
        int reason = 0;
        const CMercStrategyOrderItem *pOrderItem = sendOrder(pExec->pTryInstrument(),type,direction,price,volume, reason);
        if (pOrderItem!=NULL)
        {
            pOrderItem->m_pUser = pExec;
            pOrderItem->m_userInt2 = -1;
            pExec->tryOrderSent(pOrderItem->m_userInt1);
        }
        else
        {
            finishSpreadExec(pExec);
        }
    }
    bool sendForceOrder(CSpreadExec *pExec, CForceTask *pTask)
    {
        pTask->prepareForceOrder();
        int type = pTask->m_forceOrderType;
        int direction = pTask->m_forceOrderDirection;
        double price = pTask->m_forceOrderPrice;
        int volume = pTask->m_forceOrderVolume;
        int legID = pTask->m_legID;

        // Case when leg exp volume is 0
        if (volume == 0 && pTask->tryStop())
        {
            return false;
        }

        int reason = 1;
        const CMercStrategyOrderItem *pOrderItem = sendOrder(pExec->pForceInstrument(legID), type, direction, price, volume, reason);
        if (volume != 0 && pOrderItem!=NULL)
        {
            pOrderItem->m_pUser = pExec;
            pOrderItem->m_userInt2 = pTask->taskID();
            pTask->notifyOrderSent(pOrderItem->m_userInt1);
            return true;
        }
        else
        {
            pTask->notifyOrderSendFailed();
            if (pTask->tryStop()) 
                finishForceTask(pExec, pTask);
            return false;
        }
    }
    virtual void notifyOrder(const CMercStrategyOrderItem *pOrderItem,bool isFirstTime)
    {
        const COrder *pOrder = pOrderItem->m_pOrder;
        if (!pOrder->isFinished())
        {
            int orderType = pOrderItem->m_userInt2;
            if (orderType == -1)
            {
                setAutoCancel(pOrderItem, isFirstTime, m_env.m_tryOrderWaitTime);
            }
            else if (orderType >= 0)
            {
                setAutoCancel(pOrderItem, isFirstTime, m_env.m_forceOrderWaitTime);
            }
            else //(orderType == -2)
            {
                setAutoCancel(pOrderItem, isFirstTime, m_env.m_clearOrderWaitTime);
            }
        }
        else
        {
            checkOrderFinished(pOrderItem);
        }
    }
    void checkOrderFinished(const CMercStrategyOrderItem *pOrderItem)
    {
        const COrder *pOrder = pOrderItem->m_pOrder;
        if (pOrder!=NULL && m_orderMap.find(pOrder->getOrderRef()) != m_orderMap.end())
        {
            if (pOrder->isFinished() && pOrderItem->m_userLongLong1 == pOrder->getTradeVolume())
            {
                CSpreadExec *pExec = (CSpreadExec *)pOrderItem->m_pUser;
                int orderType = pOrderItem->m_userInt2;
                if (orderType >= 0)
                {
                    finishForceOrder(pExec, orderType, pOrder);
                }
                else if (orderType == -1)
                {
                    pExec->tryOrderFinished();
                    int tryLegPendingVlm = pExec->pendingVlm(pExec->m_tryLegID);
                    if (tryLegPendingVlm != 0)
                    {
                        sendTryOrder(pExec);
                        return;
                    }

                    if (pExec->tryStop())
                    {
                        finishSpreadExec(pExec);
                    }
                }
                unsubscribeOrder(pOrder->getOrderRef());
            }
        }
    }
    virtual void notifyTrade(const CMercStrategyOrderItem *pOrderItem, const CTrade *pTrade)
    {
        int vlm = pTrade->getVolume();
        pOrderItem->m_userLongLong1 += vlm;

        CSpreadExec *pExec = (CSpreadExec *)pOrderItem->m_pUser;
        int orderType = pOrderItem->m_userInt2;
        const CInstrument *pInst = pTrade->getInstrument();
        int instRef = pInst->getInstrumentRef();
        int trdVlm = (pTrade->getDirection() == D_Sell) ? -vlm : vlm;
        double price = pTrade->getPrice();
        if (orderType == -1)
        {
            pExec->tryOrderTraded(trdVlm, price);
            for (int i=0; i<pExec->m_pLegs.size(); i++)
            {
                if (i == pExec->m_tryLegID)
                    continue;
                startForceTask(pExec, i);
            }
        }
        else if (orderType >= 0)
        {
            int legID = pExec->getLegId(instRef);
            pExec->forceOrderTraded(legID, trdVlm, price);
        }
        else //orderType == -2
        {
            pExec->reduceRemainPositions(instRef,trdVlm);
        }
        checkOrderFinished(pOrderItem);
        
        CFutureExtentionAE *pFuture = m_pFutures[instRef];
        pFuture->notifyOpenTrade(trdVlm,price);
    }
    void finishForceOrder(CSpreadExec *pExec, int taskID, const COrder *pOrder)
    {
        CForceTask *pTask = pExec->getTask(taskID);
        if (pTask == NULL)
        {
            return;
        }
        if (pOrder->isRejected())
        {
            pTask->notifyOrderFailed();
        }
        else
        {
            int trdVlm=pOrder->getTradeVolume();
            if (pOrder->getDirection() == D_Sell)
            {
                trdVlm = -trdVlm;
            }
            pTask->notifyOrderFinish(trdVlm);
            if (pTask->needResendOrder())
            {
                sendForceOrder(pExec, pTask);
                return;
            }
        }
        if (pTask->tryStop())
        {
            finishForceTask(pExec,pTask);
        }
    }
    void unsubscribeOrder(int orderID)
    {
        if (m_orderMap.find(orderID) != m_orderMap.end())
        {
            m_orderMap.erase(orderID);
        }
    }
    void startForceTask(CSpreadExec *pExec, int legID)
    {
        int pendingVlm = pExec->pendingVlm(legID);

        int pendingTaskID = pExec->pendingTask(legID);
        CForceTask *pTask = pendingTaskID < 0? m_pForceTaskManager->getWorker(): pExec->getTask(pendingTaskID);
        CFutureExtentionAE *pLeg = pExec->m_pLegs.at(legID);
        if (pTask != NULL && !pTask->hasOrder())
        {
            pTask->start(pExec->spreadID(), pExec->m_pLegs.at(legID), legID, pendingVlm);
            sendForceOrder(pExec, pTask);
            pExec->subscribeTask(pTask, legID);
            pLeg->subscribeTask(pTask->workerID(),pTask->taskID());
            int stopTS = *m_pCurTimeStamp + m_env.m_forceTaskWaitTime;
            setTimer(stopTS,TT_ForceTaskTimeOut,pTask);
            pTask->notifyTimerSet(stopTS);
        }
    }
    void finishForceTask(CSpreadExec *pExec,CForceTask *pTask)
    {
        pExec->unsubscribeTask(pTask->taskID());
        pTask->pFuture()->unsubscribeTask(pTask->workerID());
        pTask->stop();
        if (pExec->tryStop())
        {
            finishSpreadExec(pExec);
        }
        startPendingTask();
    }
    void startPendingTask()
    {
        for (auto& it : m_pTrdSprds)
        {
            CSpreadExtentionAE *pSpread = it.second;
            CSpreadExec *pExec = pSpread->m_pSpreadExec;
            for (int i=0; i<pExec->m_pLegs.size(); i++)
            {
                if (i == pExec->m_tryLegID)
                    continue;

                if (pExec->pendingVlm(i) != 0)
                {
                    startForceTask(pExec, i);
                }
            }
        }
    }

    void finishSpreadExec(CSpreadExec *pExec)
    {
        int spreadTrdVolume = pExec->calcSpreadTrdVolume();
        if (spreadTrdVolume!=0)
        {
            double spreadTrdPrice = pExec->m_spreadAvgPrice;
            double spreadExePrice = pExec->m_sprdExeAvgPr;
            CSpreadExtentionAE *pSpread = m_pSpreads[pExec->spreadID()];
            pSpread->notifyExecFinished(spreadTrdVolume,spreadTrdPrice,*m_pCurTimeStamp, spreadExePrice);

            std::string flw;
            char flwInfo[2000];
            char memo[1000];
            sprintf(memo, "leg0-bp-ap-%g-%g|leg1-bp-ap-%g-%g|mid-%g-bp-ap-%g-%g|mkt-bp-ap-%g-%g", pSpread->m_pLegs.at(0)->BP(), pSpread->m_pLegs.at(0)->AP(), pSpread->m_pLegs.at(1)->BP(), pSpread->m_pLegs.at(1)->AP(), pSpread->m_refMid, pSpread->m_buy, pSpread->m_sell, pSpread->m_spBP, pSpread->m_spAP);
            sprintf(flwInfo, ",SPRDTRD,dt,%d,tm,%s,trddt,%d,strat,%s,sprd,%s,sprdpr,%g,direct,%d,price,%g,vlm,%d,tgt,%g,memo,%s", today(), getTimeString(m_buffer, m_env.m_pStrategy->getCurTimeStamp(), true), m_env.m_pStrategy->getTradingDay(), m_env.m_pStrategy->getStrategyName(), pSpread->m_sprdNm.c_str(), spreadTrdPrice, spreadTrdVolume>0? 1: -1, spreadExePrice, spreadTrdVolume, pExec->m_sprdTgtPr, memo);
            flw.append(flwInfo);
            m_pTrdFlw->log(flw.c_str()); 

            m_pTrdFlw->log(",SPRDPOS,dt,%d,tm,%s,trddt,%d,strat,%s,sprd,%s,pos,%d,stts,%s,mid,%g", today(), getTimeString(m_buffer, m_env.m_pStrategy->getCurTimeStamp(), true), m_env.m_pStrategy->getTradingDay(), m_env.m_pStrategy->getStrategyName(), pSpread->m_sprdNm.c_str(), pSpread->m_pSignal->m_pos, "MOD", pSpread->m_refMid);

            m_tradeCount++;
            m_tradeVolume+=abs(spreadTrdVolume);
            if (spreadTrdVolume!=pExec->m_spreadExpVlm)
            {
                m_cancelVolume+=abs(pExec->m_spreadExpVlm-spreadTrdVolume);
            }
        }
        else
        {
            m_cancelCount++;
            m_cancelVolume+=abs(pExec->m_spreadExpVlm);
        }
        if (!pExec->isBalanced())
        {
            m_failedCount++;
        }
        m_sendCount++;
        m_sendVolume+=abs(pExec->m_spreadExpVlm);
        pExec->stop();

        updateBiasSlf();
        updateConstrain();
        refreshRiskStatus();
        syncData();
    }
    void clearRemainPositions()
    {
        for (auto& it : m_pTrdSprds)
        {
            CSpreadExec *pExec = it.second->m_pSpreadExec;
            for (auto& it2 : pExec->m_remainPositions)
            {
                int id = it2.first;
                int pos = it2.second;
                CFutureExtentionAE *pFuture = m_pFutures[id];
                g_pMercLog->log("%s|%s|%s,clearRemainPositions,remain_pos,%d,hasOrder,%d",m_env.m_strategyName,m_pTradeControl->timeString(), pFuture->ID(), pos, pFuture->hasOrder());
                if (pos != 0 && !pFuture->hasOrder())
                {
                    int type = (m_env.m_clearOrderWaitTime>=0 ) ? ODT_Limit : ODT_FAK;
                    int direction = (-pos>0) ? D_Buy : D_Sell;
                    bool hasCounterVolume = false;
                    double price = 0.0;
                    double adjPrice = pFuture->tick()*m_env.m_clearOrderPriceAdjustTicks;
                    if (-pos > 0)
                    {
                        hasCounterVolume = pFuture->AQ()>0;
                        price = std::min(pFuture->upperLimitPrice(),pFuture->AP()+adjPrice);
                    }
                    else
                    {
                        hasCounterVolume = pFuture->BQ()>0;
                        price = std::max(pFuture->lowerLimitPrice(),pFuture->BP()-adjPrice);
                    }
                    g_pMercLog->log("%s|%s|%s,clearRemainPositions,remain_pos,%d,hasOrder,%d,hsCntrVlm,%d",m_env.m_strategyName,m_pTradeControl->timeString(), pFuture->ID(), pos, pFuture->hasOrder(), hasCounterVolume);
                    if (hasCounterVolume)
                    {
                        int reason = 2;
                        const CMercStrategyOrderItem *pOrderItem = sendOrder(pFuture->pInstrument(),type,direction,price,abs(pos), reason);
                        if (pOrderItem!=NULL)
                        {
                            pOrderItem->m_pUser = pExec;
                            pOrderItem->m_userInt2 = -2;
                            g_pMercLog->log("%s,clearRemainPosition order sent",m_env.m_strategyName);
                        }
                        else
                        {
                            g_pMercLog->log("%s,clearRemainPosition order failed",m_env.m_strategyName);
                        }
                    }
                }
            }
        }
    }

    virtual void onTime(int timeStamp, int type, void *pUser)
    {
        m_pTradeControl->internalOnTime(timeStamp,type);
        switch(type)
        {
        case TT_OnlyClose:
            onOnlyClose();
            break;
        case TT_DaySettle:
            onDaySettle();
            break;
        case TT_DayEnd:
            onDayEnd();
            break;
        case TT_NtEnd:
            onNtEnd();
            break;
        case TT_Period:
            onPeriod();

            if (m_env.m_logTrdFlw != 0)
            {
                for (auto& it : m_pTrdSprds)
                {
                    logTrds(typeid(this).name(), "Period", "Both", it.second, true, 0, 0, 0, 0);
                }
            }
            break;
        case TT_ForceTaskTimeOut:
            onForceTaskTimeOut(timeStamp,pUser);
            break;
        default:
            break;
        }
    }
    void onForceTaskTimeOut(int timeStamp,void *pUser)
    {
        CForceTask *pTask = (CForceTask *)pUser;
        if(pTask!=NULL && pTask->checkTimeout(timeStamp))
        {
            if (pTask->hasOrder())
            {
                int orderID = pTask->orderID();
                if (m_orderMap.find(orderID)!=m_orderMap.end())
                {
                    cancelOrderItem(m_orderMap[orderID]);
                }
            }
            else
            {
                int spreadID = pTask->spreadID();
                if (m_pSpreads.find(spreadID) != m_pSpreads.end())
                {
                    CSpreadExtentionAE *pSpread = m_pSpreads[spreadID];
                    CSpreadExec *pExec = pSpread->m_pSpreadExec;
                    finishForceTask(pExec,pTask);
                }
            }
        }
    }
    void cancelOrderItem(const CMercStrategyOrderItem *pOrderItem)
    {
        const COrder *pOrder = pOrderItem->m_pOrder;
        if (pOrder!=NULL && !pOrder->isFinished())
        {
            cancelOrder(pOrderItem);
        }
    }
    void cancelAll()
    {
        for (auto& it : m_orderMap)
        {
            cancelOrderItem(it.second);
        }
    }

    void onPeriod()
    {
        if (m_needOnBar && m_pTradeControl->inSession(*m_pCurTimeStamp-1000))
        {
            for (auto& it : m_pFutures)
            {
                it.second->onBar();
            }

            for (auto& it : m_pSpreads)
            {
                it.second->calcMean();
            }

            for (auto& itA : m_pTrdSprds)
            {
                itA.second->updateEdge();
                itA.second->refreshPnlStatus();
            }

            updateBiasSlf();
            updateConstrain();
            refreshRiskStatus();
            if(m_pTradeControl->getTradeConstrain()<4)
            {
                clearRemainPositions();
                /* g_pMercLog->log("%s|%s,onPeriod,pTradeControl->getTradeConstrain,%d<4,clearRemainPositions",m_env.m_strategyName,m_pTradeControl->timeString(), m_pTradeControl->getTradeConstrain()); */
            }

            if (m_env.m_needFuzzySort>0)
            {
                updateInstTriggerMap();
            }

            m_needOnBar = false;
            //g_pMercLog->log("%s|%s|onBar",m_env.m_strategyName,m_pTradeControl->timeString());
            
            syncData();
        }
    }

    void logTrds(const char* clsID, const char* odrOrTrd, const char* whichLeg, CSpreadExtentionAE *pSprd, bool isSell, double trgrPr, double pr, int vlm, double exePr)
    {
        int pos = pSprd->m_pSignal->m_pos;

        double biasBuy = DBL_MAX;
        double biasSell = DBL_MAX;

        pSprd->updtBuySell(biasBuy, biasSell);

        double sprdMrgn = pSprd->m_ttlMrgn;

        std::string logStr;
        char logChrs[2000];
        sprintf(logChrs, ",StratID,%s,CmbNm,%s,Date,%d,Time,%s,Pos,%d,NmPos,%d,TrdRt,%d,GodRt,%d,\
trgrPr,%g,Pr,%g,ExePr,%g,Vlm,%d,OdrOrTrd,%s,BP,%g,AP,%g,\
gridBV,%d,QuoteB,%g,TheoB,%g,TheoA,%g,QuoteA,%g,GridAV,%d,RefB,%g,RefA,%g,\
Avg,%g,RefMid,%g,StepSz,%d",
            m_env.m_strategyName, pSprd->m_sprdNm.c_str(), m_env.m_pStrategy->getTradingDay(), getTimeString(m_buffer, m_env.m_pStrategy->getCurTimeStamp(), true), pos, pSprd->m_sprdNmPos, pSprd->m_selfConstrain, m_pTradeControl->getTradeConstrain(),
            trgrPr, pr, exePr, vlm, odrOrTrd, pSprd->m_spBP, pSprd->m_spAP, 
            100, pSprd->m_buy, biasBuy, biasSell, pSprd->m_sell, 100, pSprd->m_refBuy, pSprd->m_refSell,
            pSprd->m_spAvg, pSprd->m_refMid, pSprd->m_stepSize);
        logStr.append(logChrs);

        logStr.append(",Poss,");
        for (auto it: m_pTrdSprds)
        {
            CSpreadSignal *pSig = nullptr;
            if (it.second->m_coefs.size() > 2 || pSprd->m_coefs.size() > 2)
                continue;

            pSig = getSprdSignal(pSprd, it.second);

            if (pSig != nullptr)
            {
                if (it.second->m_pLegs.size() > 1)
                {
                    sprintf(logChrs, "/%s/ %d /", it.second->m_sprdNm.c_str(), it.second->getPos());
                    logStr.append(logChrs);
                }
            }
        }

        sprintf(logChrs, ",MaxLot,%d,SprdMaxMrgn,%g,SprdMrgnPct,%g,Mrgn,%g,MaxLoss,%g,\
mrgnU,%g, StratMrgn,%g,AcctAvlb,%g,AcctMinAvlb,%g",
            pSprd->m_maxTradeSize, pSprd->m_maxAmt, pSprd->m_mrgnPct, sprdMrgn, 0.0,
            pSprd->m_marginPerPair, m_totalMargin, getNetAvailable(m_pAccountManager), m_env.m_minAvailable);
        logStr.append(logChrs);

        m_pTrdLog->log(logStr.c_str());
    }

    void onOnlyClose()
    {
        for(auto& it : m_pTrdSprds)
        {
            if (it.second->m_selfConstrain == 1)
            {
                it.second->addConstrain(2);
            }
        }
    }
    void onDaySettle()
    {
        for(auto& it : m_pFutures)
        {
            if (!it.second->hasOrder())
            {
                dailySettle(it.first);
            }
        }
    }
    void dailySettle(int id)
    {
        CFutureExtentionAE *pFuture = m_pFutures[id];
        if (!pFuture->isSettled())
        {
            pFuture->settle();
            pFuture->settled();
        }
    }
    void onDayEnd()
    {
        updateConstrain();
        for (auto& it : m_pSpreads)
        {
            logTrds(typeid(this).name(), "EOD", "Both", it.second, true, 0, 0, 0, 0);

            m_pTrdFlw->log(",SPRDPOS,dt,%d,tm,%s,trddt,%d,strat,%s,sprd,%s,pos,%d,stts,%s", today(), getTimeString(m_buffer, m_env.m_pStrategy->getCurTimeStamp(), true), m_env.m_pStrategy->getTradingDay(), m_env.m_pStrategy->getStrategyName(), it.second->m_sprdNm.c_str(), it.second->m_pSignal->m_pos, "EOD");
        }
        syncData();
    }
    void onNtEnd()
    {
        updateConstrain();
        for (auto& it : m_pSpreads)
        {
            logTrds(typeid(this).name(), "EON", "Both", it.second, true, 0, 0, 0, 0);

            m_pTrdFlw->log(",SPRDPOS,dt,%d,tm,%s,trddt,%d,strat,%s,sprd,%s,pos,%d,stts,%s", today(), getTimeString(m_buffer, m_env.m_pStrategy->getCurTimeStamp(), true), m_env.m_pStrategy->getTradingDay(), m_env.m_pStrategy->getStrategyName(), it.second->m_sprdNm.c_str(), it.second->m_pSignal->m_pos, "EON");
        }
        syncData();
    }
    virtual void notifyTradeSegment(int timeStamp)
    {
    }
    virtual void notifyFreeTime(void)
    {
    }
    virtual const char *handleCommand(const CMercStrategyCommand *pCommand)
    {
        const char *msg=internalHandleCommand(pCommand);
        m_env.refreshParameterStatus();
        updateConstrain();
        refreshRiskStatus();
        return msg;
    }

    const char *internalHandleCommand(const CMercStrategyCommand *pCommand)
    {
        switch (pCommand->CommandID)
        {
        case 0:
            return chgRefMid(pCommand);
        case 1:
            return chgSprdMaxLot(pCommand);
        case 2:
            return chgSprdStpSz(pCommand);
        case 3:
            return changeEnableTrade(pCommand);
        case 4:
            return changeTryOrderWaitTime(pCommand);
        case 5:
            return changeTryOrderPriceAdj(pCommand);
        case 6:
            return changeForceOrderWaitTime(pCommand);
        case 7:
            return changeForceTaskTimeOut(pCommand);
        case 8:
            return changeForceOrderStartPriceAdj(pCommand);
        case 9:
            return changeForceOrderStepAdjAfterMD(pCommand);
        case 10:
            return changeAdjPosStep(pCommand);
        case 11:
            return changeMinAvailable(pCommand);
        case 12:
            return changeOffsetStrategy(pCommand);
        case 13:
            return refreshStatus();
        default:
            return "未知命令";
        }
    }
    CSpreadExtentionAE *cmd2sprdFast(const CMercStrategyCommand *pCmd)
    {
        int sprdIdx = pCmd->IntValue[0];

        if (m_pSpreads.find(sprdIdx) == m_pSpreads.end())
            return nullptr;
        else
            return m_pSpreads[sprdIdx];
    }
    const char *chgRefMid(const CMercStrategyCommand *pCmd)
    {
        double newVal = pCmd->FloatValue[0];
        CSpreadExtentionAE *pSprd = cmd2sprdFast(pCmd);

        if (pSprd == nullptr)
            return "Wrong instruments";
        else
        {
            double oldVal = pSprd->m_refMid;
            pSprd->m_refMid = pSprd->m_pSignal->m_refMid = newVal;
            // re-price
            pSprd->updtBuySell(pSprd->m_buy, pSprd->m_sell);
            g_pMercLog->log("%s,handleCommand,chgRefMid %g->%g", m_env.m_strategyName, oldVal, pSprd->m_refMid);
            refreshStatus();
            syncData();
        }
        return NULL;
    }
    const char *chgSprdMaxLot(const CMercStrategyCommand *pCmd)
    {
        double newVal = pCmd->FloatValue[0];
        CSpreadExtentionAE *pSprd = cmd2sprdFast(pCmd);

        if (pSprd == nullptr)
            return "Wrong instruments";
        else
        {
            double oldVal = pSprd->m_manSprdMaxLot;
            pSprd->m_manSprdMaxLot = pSprd->m_pSignal->m_sprdMaxLot = newVal;
            pSprd->adjMaxAmt();
            pSprd->adjustPositionLimit(pSprd->m_manSprdMaxLot);

            g_pMercLog->log("%s,handleCommand,chgSprdMaxLot %g->%g", m_env.m_strategyName, oldVal, newVal);
            refreshStatus();
            syncData();
        }
        return NULL;
    }
    const char *chgSprdStpSz(const CMercStrategyCommand *pCmd)
    {
        double newVal = pCmd->FloatValue[0];
        CSpreadExtentionAE *pSprd = cmd2sprdFast(pCmd);

        if (pSprd == nullptr)
            return "Wrong instruments";
        else
        {
            double oldVal = pSprd->m_stepSize;
            pSprd->m_stepSize = pSprd->m_pSignal->m_stepSize = newVal;
            pSprd->updtStepSize();
            updateBiasSlf();

            g_pMercLog->log("%s,handleCommand,chgSprdStpSz %g->%g", m_env.m_strategyName, oldVal, newVal);
            refreshStatus();
            syncData();
        }
        return NULL;
    }
    const char *changeEnableTrade(const CMercStrategyCommand *pCommand)
    {
        g_pMercLog->log("%s,handleCommand,enableTrade %d->%d",
                        m_env.m_strategyName,m_env.m_enableTrade,pCommand->Index[0]);
        m_env.m_enableTrade = pCommand->Index[0];
        return NULL;
    }
    const char *changeTryOrderWaitTime(const CMercStrategyCommand *pCommand)
    {
        g_pMercLog->log("%s,handleCommand,tryOrderWaitTime %d->%d",
                        m_env.m_strategyName,m_env.m_tryOrderWaitTime,pCommand->Index[0]);
        m_env.m_tryOrderWaitTime = pCommand->Index[0];
        return NULL;
    }
    const char *changeTryOrderPriceAdj(const CMercStrategyCommand *pCommand)
    {
        g_pMercLog->log("%s,handleCommand,tryOrderPriceAdj %d->%d",
                        m_env.m_strategyName,m_env.m_tryOrderPriceAdjustTicks,pCommand->Index[0]);
        m_env.m_tryOrderPriceAdjustTicks = pCommand->Index[0];
        return NULL;
    }
    const char *changeForceOrderWaitTime(const CMercStrategyCommand *pCommand)
    {
        g_pMercLog->log("%s,handleCommand,forceOrderWaitTime %d->%d",
                        m_env.m_strategyName,m_env.m_forceOrderWaitTime,pCommand->Index[0]);
        m_env.m_forceOrderWaitTime = pCommand->Index[0];
        return NULL;
    }
    const char *changeForceTaskTimeOut(const CMercStrategyCommand *pCommand)
    {
        g_pMercLog->log("%s,handleCommand,forceTaskWaitTime %d->%d",
                        m_env.m_strategyName,m_env.m_forceTaskWaitTime,pCommand->Index[0]);
        m_env.m_forceTaskWaitTime = pCommand->Index[0];
        return NULL;
    }
    const char *changeForceOrderStartPriceAdj(const CMercStrategyCommand *pCommand)
    {
        g_pMercLog->log("%s,handleCommand,startPriceAdj %d->%d",
                        m_env.m_strategyName,m_env.m_startPriceAdjustTicks,pCommand->Index[0]);
        m_env.m_startPriceAdjustTicks = pCommand->Index[0];
        return NULL;
    }
    const char *changeForceOrderStepAdjAfterMD(const CMercStrategyCommand *pCommand)
    {
        g_pMercLog->log("%s,handleCommand,stepAdjAfterMD %d->%d",
                        m_env.m_strategyName,m_env.m_stepPriceAdjustTicksAfterMD,pCommand->Index[0]);
        m_env.m_stepPriceAdjustTicksAfterMD = pCommand->Index[0];
        return NULL;
    }
    const char *changeAdjPosStep(const CMercStrategyCommand *pCommand)
    {
        g_pMercLog->log("%s,handleCommand,adjPosStep %d->%d",
                        m_env.m_strategyName,m_env.m_adjPosStep,pCommand->Index[0]);
        m_env.m_adjPosStep = pCommand->Index[0];
        controlPositionLimit();
        updateBiasSlf();
        return NULL;
    }
    const char *changeMinAvailable(const CMercStrategyCommand *pCommand)
    {
        if (pCommand->FloatValue[0] <= 0)
        {
            return "minAvailable must > 0";
        }
        g_pMercLog->log("%s,handleCommand,minAvailable %g->%g",
                        m_env.m_strategyName,m_env.m_minAvailable,pCommand->FloatValue[0]);
        m_env.m_minAvailable = pCommand->FloatValue[0];
        return NULL;
    }
    const char *changeOffsetStrategy(const CMercStrategyCommand *pCommand)
    {
        g_pMercLog->log("%s,handleCommand,offsetStrategy %d->%d",
                        m_env.m_strategyName,m_env.m_offsetStrategy,pCommand->Index[0]);
        m_env.m_offsetStrategy = pCommand->Index[0];
        return NULL;
    }
    const char *refreshStatus()
    {
        for (auto& it : m_pTrdSprds)
        {
            it.second->refreshAllStatus();
        }
        return NULL;
    }
};
EndMercStrategy(EZDG, "20251209")


#endif
