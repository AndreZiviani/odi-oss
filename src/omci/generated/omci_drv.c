#include "omci_drv.h"

int omci_setLog(uint32_t a0)
{
	uint8_t buf[4] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	return omci_drv_call(1u, buf, 4u);
}

int omci_setDevMode(uint32_t a0)
{
	uint8_t buf[4] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	return omci_drv_call(2u, buf, 4u);
}

int omci_getDevCapabilities(void *buf)
{
	return omci_drv_call(3u, buf, 120u);
}

int omci_getDevIdVersion(void *buf)
{
	return omci_drv_call(4u, buf, 40u);
}

int omci_setDualMgmtMode(uint32_t a0)
{
	uint8_t buf[4] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	return omci_drv_call(5u, buf, 4u);
}

int omci_getDrvVersion(void *buf)
{
	return omci_drv_call(6u, buf, 64u);
}

int omci_setWanQueueNum(uint32_t a0)
{
	uint8_t buf[4] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	return omci_drv_call(7u, buf, 4u);
}

int omci_setPortRemp(void *buf)
{
	return omci_drv_call(8u, buf, 128u);
}

int omci_getUsDBRuStatus(void *buf)
{
	return omci_drv_call(9u, buf, 4u);
}

int omci_getTransceiverStatus(void *buf)
{
	return omci_drv_call(10u, buf, 36u);
}

int omci_setSignalParameter(void *buf)
{
	return omci_drv_call(11u, buf, 8u);
}

int omci_setRDIMode(uint32_t a0)
{
	uint8_t buf[4] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	return omci_drv_call(12u, buf, 4u);
}

int omci_getOnuState(void *buf)
{
	return omci_drv_call(13u, buf, 4u);
}

int omci_setSerialNum(void *buf)
{
	return omci_drv_call(14u, buf, 9u);
}

int omci_getSerialNum(void *buf)
{
	return omci_drv_call(15u, buf, 9u);
}

int omci_setGponPasswd(void *buf)
{
	return omci_drv_call(16u, buf, 10u);
}

int omci_activateGpon(uint32_t a0)
{
	uint8_t buf[4] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	return omci_drv_call(17u, buf, 4u);
}

int omci_setGemBlkLen(uint32_t a0)
{
	uint8_t buf[2] = { 0 };

	buf[0] = (uint8_t)(a0 >> 8);  buf[1] = (uint8_t)a0;
	return omci_drv_call(18u, buf, 2u);
}

int omci_getGemBlkLen(void *buf)
{
	return omci_drv_call(19u, buf, 2u);
}

int omci_setPonBwThreshold(void *buf)
{
	return omci_drv_call(20u, buf, 8u);
}

int omci_updateGemFlow(void)
{
	uint8_t buf[68] = { 0 };

	buf[64] = (uint8_t)(2u >> 24); buf[65] = (uint8_t)(2u >> 16);
	buf[66] = (uint8_t)(2u >> 8);  buf[67] = (uint8_t)2u;
	return omci_drv_call(25u, buf, 68u);
}

int omci_updateUsGemFlow(void)
{
	uint8_t buf[68] = { 0 };

	buf[64] = (uint8_t)(1u >> 24); buf[65] = (uint8_t)(1u >> 16);
	buf[66] = (uint8_t)(1u >> 8);  buf[67] = (uint8_t)1u;
	return omci_drv_call(25u, buf, 68u);
}

int omci_setDsBcGemFlow(uint32_t a0)
{
	uint8_t buf[4] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	return omci_drv_call(26u, buf, 4u);
}

int omci_setForceEmergencyStop(uint32_t a0)
{
	uint8_t buf[4] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	return omci_drv_call(27u, buf, 4u);
}

int omci_getPortLinkStatus(void *buf)
{
	return omci_drv_call(28u, buf, 8u);
}

int omci_getPortSpeedDuplexStatus(void *buf)
{
	return omci_drv_call(29u, buf, 12u);
}

int omci_setPortAutoNegoAbility(void *buf)
{
	return omci_drv_call(30u, buf, 8u);
}

int omci_getPortAutoNegoAbility(void *buf)
{
	return omci_drv_call(31u, buf, 8u);
}

int omci_setPortState(void *buf)
{
	return omci_drv_call(32u, buf, 8u);
}

int omci_getPortState(void *buf)
{
	return omci_drv_call(33u, buf, 8u);
}

int omci_setMaxFrameSize(void *buf)
{
	return omci_drv_call(34u, buf, 8u);
}

int omci_getMaxFrameSize(void *buf)
{
	return omci_drv_call(35u, buf, 8u);
}

int omci_setPhyLoopback(void *buf)
{
	return omci_drv_call(36u, buf, 8u);
}

int omci_getPhyLoopback(void *buf)
{
	return omci_drv_call(37u, buf, 8u);
}

int omci_setPhyPwrDown(void *buf)
{
	return omci_drv_call(38u, buf, 8u);
}

int omci_getPhyPwrDown(void *buf)
{
	return omci_drv_call(39u, buf, 8u);
}

int omci_setPauseControl(void *buf)
{
	return omci_drv_call(40u, buf, 8u);
}

int omci_getPauseControl(void *buf)
{
	return omci_drv_call(41u, buf, 8u);
}

int omci_getPortStat(void *buf)
{
	return omci_drv_call(42u, buf, 200u);
}

int omci_resetPortStat(uint32_t a0)
{
	uint8_t buf[4] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	return omci_drv_call(43u, buf, 4u);
}

int omci_getUsFlowStat(void *buf)
{
	return omci_drv_call(44u, buf, 16u);
}

int omci_resetUsFlowStat(uint32_t a0)
{
	uint8_t buf[4] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	return omci_drv_call(45u, buf, 4u);
}

int omci_getDsFlowStat(void *buf)
{
	return omci_drv_call(46u, buf, 16u);
}

int omci_resetDsFlowStat(uint32_t a0)
{
	uint8_t buf[4] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	return omci_drv_call(47u, buf, 4u);
}

int omci_getDsFecStat(void *buf)
{
	return omci_drv_call(48u, buf, 12u);
}

int omci_deactiveBdgConn(uint32_t a0)
{
	uint8_t buf[4] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	return omci_drv_call(50u, buf, 4u);
}

int omci_setDscpRemap(void *buf)
{
	return omci_drv_call(52u, buf, 64u);
}

int omci_setMacLearnLimit(uint32_t a0, uint32_t a1)
{
	uint8_t buf[8] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	buf[4] = (uint8_t)(a1 >> 24); buf[5] = (uint8_t)(a1 >> 16);
	buf[6] = (uint8_t)(a1 >> 8);  buf[7] = (uint8_t)a1;
	return omci_drv_call(53u, buf, 8u);
}

int omci_setMacFilter(void *buf)
{
	return omci_drv_call(54u, buf, 28u);
}

int omci_setGroupMacFilter(void *buf)
{
	return omci_drv_call(55u, buf, 24u);
}

int omci_setSvlanTpid(uint32_t a0, uint32_t a1)
{
	uint8_t buf[8] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	buf[4] = (uint8_t)(a1 >> 24); buf[5] = (uint8_t)(a1 >> 16);
	buf[6] = (uint8_t)(a1 >> 8);  buf[7] = (uint8_t)a1;
	return omci_drv_call(56u, buf, 8u);
}

int omci_getSvlanTpid(uint32_t a0)
{
	uint8_t buf[8] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	return omci_drv_call(57u, buf, 8u);
}

int omci_getCvlanState(void *buf)
{
	return omci_drv_call(58u, buf, 4u);
}

int omci_setDot1RateLimiter(void *buf)
{
	return omci_drv_call(59u, buf, 20u);
}

int omci_delDot1RateLimiter(void *buf)
{
	return omci_drv_call(60u, buf, 20u);
}

int omci_getBridgeTableByPort(void *buf)
{
	return omci_drv_call(61u, buf, 4u);
}

int omci_setAgeingTime(uint32_t a0)
{
	uint8_t buf[4] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	return omci_drv_call(62u, buf, 4u);
}

int omci_setPortBridging(uint32_t a0)
{
	uint8_t buf[4] = { 0 };

	buf[0] = (uint8_t)(a0 >> 24); buf[1] = (uint8_t)(a0 >> 16);
	buf[2] = (uint8_t)(a0 >> 8);  buf[3] = (uint8_t)a0;
	return omci_drv_call(63u, buf, 4u);
}

int omci_setFloodingPortMask(void *buf)
{
	return omci_drv_call(64u, buf, 12u);
}

int omci_setUniPortRate(void *buf)
{
	return omci_drv_call(67u, buf, 12u);
}

int omci_sendOmciEvent(void *buf)
{
	return omci_drv_call(69u, buf, 12u);
}

int omci_setTodInfo(void *buf)
{
	return omci_drv_call(70u, buf, 14u);
}

int omci_setUniQos(void *buf)
{
	return omci_drv_call(71u, buf, 76u);
}

