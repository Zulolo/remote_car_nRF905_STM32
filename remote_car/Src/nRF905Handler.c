#include "main.h"
#include "stm32f1xx_hal.h"
#include "cmsis_os.h"

#include <stdlib.h>
#include <string.h>

extern osThreadId nRF905HandlerHandle;
extern osTimerId nCarStatusHandle;
extern SPI_HandleTypeDef hspi1;
extern osMutexId nRF905OccupyHandle;
extern osSemaphoreId nRF905SPIDMACpltHandle;
extern osSemaphoreId DataReadySetHandle;

#define NRF905_SPI_CHN							hspi1
#define NRF905_POWER							3

#define NRF905_TX_ADDR_LEN						4
#define NRF905_RX_ADDR_LEN						4
#define NRF905_RX_PAYLOAD_LEN					32
#define NRF905_TX_PAYLOAD_LEN					NRF905_RX_PAYLOAD_LEN
#define NRF905_SPI_TX_RX_MAX_LEN				(NRF905_RX_PAYLOAD_LEN + 1)
#define MAX_HOPPING_RETRY_TIMES					3
#define TEST_NRF905_TX_ADDR						0x12345678
#define TEST_NRF905_RX_ADDR						0x87654321

/* Here is how the RF works:
 * UP keeps monitoring if there is any valid frame available (CD should be SET) on certain channel for certain time.
 * If yes, receive frame and continuously monitoring on this channel. If no, hop to next channel according to the hopping table.
 *
 * Down keeps transmitting frames every certain ms. If transmitting failed, (can not get valid response) down start hopping procedure.
 * Hopping procedure is to trying burst transmitting ACK frame continuously.
 * If transmitting failed, jump to next channel according to table and start to transmit again.
 * The TX&RX address is generated by some algorithm and is set at start up or during hopping
 *
 */
#define NRF905_RX_ADDRESS_IN_CR					5
#define NRF905_CMD_WC_MASK						0x0F
#define NRF905_CMD_WC(unWR_CFG_ByteIndex)		((unWR_CFG_ByteIndex) & NRF905_CMD_WC_MASK)	// Write Configuration register
#define NRF905_CMD_RC_MASK						0x0F
#define NRF905_CMD_RC(unRD_CFG_ByteIndex)		(((unRD_CFG_ByteIndex) & NRF905_CMD_RC_MASK) | 0x10)	// Read Configuration register
#define NRF905_CMD_WTP							0x20
#define NRF905_CMD_RTP							0x21
#define NRF905_CMD_WTA							0x22
#define NRF905_CMD_RTA							0x23
#define NRF905_CMD_RRP							0x24
#define NRF905_CMD_CC(unPwrChn)					((unPwrChn) | 0x8000)
#define CH_MSK_IN_CC_REG						0x01FF
#define NRF905_DR_IN_STATUS_REG(status)			((status) & (0x01 << 5))

#define GET_LENGTH_OF_ARRAY(x) 					(sizeof(x)/sizeof(x[0]))
typedef enum _nRF905Command {
	NRF905_CMD_GET_STATUS = 0,
	NRF905_CMD_SET_STATUS,
	NRF905_CMD_MAX
} nRF905CMD_t;

typedef struct _CarStatus {
	int16_t nFrontSpeed;
	int16_t nRearSpeed;
	int16_t nSteer;
} CarStatus_t;
static CarStatus_t tRemoteCarStatus;

typedef enum _nRF905Modes {
	NRF905_MODE_PWR_DOWN = 0,
	NRF905_MODE_STD_BY,
	NRF905_MODE_BURST_RX,
	NRF905_MODE_BURST_TX,
	NRF905_MODE_MAX
} nRF905Mode_t;

typedef struct _nRF905PinLevelInMode {
	GPIO_PinState nPWR_UP_PIN;
	GPIO_PinState nTRX_CE_PIN;
	GPIO_PinState nTX_EN_PIN;
} nRF905PinLevelInMode_t;

// Pin status according to each nRF905 mode
static const nRF905PinLevelInMode_t unNRF905MODE_PIN_LEVEL[] = {
		{ GPIO_PIN_RESET, GPIO_PIN_RESET, GPIO_PIN_RESET},
		{ GPIO_PIN_SET, GPIO_PIN_RESET, GPIO_PIN_RESET },
		{ GPIO_PIN_SET, GPIO_PIN_SET, GPIO_PIN_RESET },
		{ GPIO_PIN_SET, GPIO_PIN_SET, GPIO_PIN_SET } };

typedef struct _nRF905Status {
	uint32_t unNRF905RecvFrameCNT;
	uint32_t unNRF905SendFrameCNT;
	uint32_t unNRF905HoppingCNT;
	uint32_t unNRF905TxAddr;
	uint32_t unNRF905RxAddr;
	uint16_t unNRF905CHN_PWR;
	nRF905Mode_t tNRF905CurrentMode;
}nRF905Status_t;

static nRF905Status_t tNRF905Status = {0, 0, 0, 0, 0, NRF905_MODE_PWR_DOWN};

// MSB of CH_NO will always be 0
static const uint8_t NRF905_CR_DEFAULT[] = { 0x4C, 0x0C, // F=(422.4+(0x6C<<1)/10)*1; No retransmission; +6db; NOT reduce receive power
		(NRF905_RX_ADDR_LEN << 4) | NRF905_TX_ADDR_LEN,	// 4 bytes RX & TX address;
		NRF905_RX_PAYLOAD_LEN, NRF905_TX_PAYLOAD_LEN, // 32 bytes RX & TX package length;
		0x00, 0x0C, 0x40, 0x08,	// RX address is the calculation result of CH_NO
		0x58 };	// 16MHz crystal; enable CRC; CRC16

static const uint16_t unCAR_REMOTE_HOPPING_TAB[] = { 0x804C, 0x803A, 0x8046, 0x8032, 0x804A, 0x8035,
		0x804B, 0x8037, 0x804F, 0x803E, 0x8047, 0x8038, 0x8044, 0x8034, 0x8043, 0x8034, 0x804B,
		0x8039, 0x804D, 0x803A, 0x804E, 0x803C, 0x8032, 0x803F };

static int32_t setNRF905Mode(nRF905Mode_t tNRF905Mode) {
	if (tNRF905Mode >= NRF905_MODE_MAX){
		return (-1);
	}
	if (tNRF905Mode == tNRF905Status.tNRF905CurrentMode){
		return 0;
	}

	HAL_GPIO_WritePin(NRF905_TX_EN_GPIO_Port, NRF905_TX_EN_Pin, unNRF905MODE_PIN_LEVEL[tNRF905Mode].nTX_EN_PIN);
	HAL_GPIO_WritePin(NRF905_TRX_CE_GPIO_Port, NRF905_TRX_CE_Pin, unNRF905MODE_PIN_LEVEL[tNRF905Mode].nTRX_CE_PIN);
	HAL_GPIO_WritePin(NRF905_PWR_UP_GPIO_Port, NRF905_PWR_UP_Pin, unNRF905MODE_PIN_LEVEL[tNRF905Mode].nPWR_UP_PIN);
	tNRF905Status.tNRF905CurrentMode = tNRF905Mode;

	return 0;
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi) {
	if (&hspi1 == hspi) {
		osSemaphoreRelease(nRF905SPIDMACpltHandle);
	}
}

static int32_t nRF905SPIDataRW(SPI_HandleTypeDef* pSPI_Handler, uint8_t* pTxBuff, uint8_t* pRxBuff, uint8_t unBuffLen) {
	int32_t nWaitResult;
	nRF905Mode_t tPreMode;
	if (unBuffLen > NRF905_SPI_TX_RX_MAX_LEN) {
		return (-1);
	}
	tPreMode = tNRF905Status.tNRF905CurrentMode;
	setNRF905Mode(NRF905_MODE_STD_BY);
	HAL_GPIO_WritePin(NRF905_CSN_GPIO_Port, NRF905_CSN_Pin, GPIO_PIN_RESET);
	if (HAL_OK == HAL_SPI_TransmitReceive_DMA(pSPI_Handler, pTxBuff, pRxBuff, unBuffLen)) {
		nWaitResult = osSemaphoreWait( nRF905SPIDMACpltHandle, 50 );
		HAL_GPIO_WritePin(NRF905_CSN_GPIO_Port, NRF905_CSN_Pin, GPIO_PIN_SET);
		setNRF905Mode(tPreMode);
		if( nWaitResult == osOK ) {
			/* The transmission ended as expected. */
			return 0;
		} else {
			/* The call to ulTaskNotifyTake() timed out. */
			return (-1);
		}
	} else {
		return (-1);
	}
}

static int32_t nRF905SPIWrite(uint8_t unCMD, const uint8_t *pData, int32_t nDataLen) {
	static uint8_t unTxBuff[NRF905_SPI_TX_RX_MAX_LEN];
	static uint8_t unRxBuff[NRF905_SPI_TX_RX_MAX_LEN];
	if (nDataLen > (NRF905_SPI_TX_RX_MAX_LEN - 1)) {
		return (-1);
	} else {
		unTxBuff[0] = unCMD;
		memcpy(unTxBuff + 1, pData, nDataLen);
		return nRF905SPIDataRW(&NRF905_SPI_CHN, unTxBuff, unRxBuff, nDataLen + 1);
	}
}

static int32_t nRF905SPIRead(uint8_t unCMD, uint8_t *pData, int32_t nDataLen) {
	static uint8_t unTxBuff[NRF905_SPI_TX_RX_MAX_LEN];
	static uint8_t unRxBuff[NRF905_SPI_TX_RX_MAX_LEN];
	int32_t nRslt;
	if (nDataLen > (NRF905_SPI_TX_RX_MAX_LEN - 1)) {
		return (-1);
	} else {
		unTxBuff[0] = unCMD;
		nRslt = nRF905SPIDataRW(&NRF905_SPI_CHN, unTxBuff, unRxBuff, nDataLen + 1);
		if (0 == nRslt) {
			memcpy(pData, unRxBuff + 1, nDataLen);
		}
		return nRslt;
	}
}

static int32_t readRxPayload(uint8_t* pBuff, int32_t nBuffLen) {
	return nRF905SPIRead(NRF905_CMD_RRP, pBuff, nBuffLen);
}

static int32_t readConfig(uint8_t unConfigAddr, uint8_t* pBuff, int32_t nBuffLen) {
	return nRF905SPIRead(NRF905_CMD_RC(unConfigAddr), pBuff, nBuffLen);
}

static int32_t writeConfig(uint8_t unConfigAddr, const uint8_t* pBuff, int32_t nBuffLen) {
	return nRF905SPIWrite(NRF905_CMD_WC(unConfigAddr), pBuff, nBuffLen);
}

static int32_t writeTxAddr(uint32_t unTxAddr) {
	return nRF905SPIWrite(NRF905_CMD_WTA, (uint8_t*)(&unTxAddr), sizeof(uint32_t));
}

static int32_t writeRxAddr(uint32_t unRxAddr) {
	return nRF905SPIWrite(NRF905_RX_ADDRESS_IN_CR, (uint8_t*)(&unRxAddr), sizeof(uint32_t));
}

// TX and RX address are already configured during hopping
static int32_t writeTxPayload(uint8_t* pBuff, int32_t nBuffLen) {
	return nRF905SPIWrite(NRF905_CMD_WTP, pBuff, nBuffLen);
}

static int32_t writeFastConfig(uint16_t unPA_PLL_CHN) {
	uint8_t unSubCmd = (uint8_t)(unPA_PLL_CHN & 0xFF);
	return nRF905SPIWrite(NRF905_CMD_CC((uint8_t)(unPA_PLL_CHN >> 8)), &unSubCmd, 1);
}

int32_t nRF905DataReadyHandler(void) {
	osSemaphoreRelease(DataReadySetHandle);
	return 0;
}

static int32_t nRF905CRInitial(void) {
	return writeConfig(0, NRF905_CR_DEFAULT, sizeof(NRF905_CR_DEFAULT));
}

#define GET_CHN_PWR_FAST_CONFIG(x, y) 		((x) | ((y) << 10))
#define GET_TX_ADDR_FROM_CHN_PWR(x)			(((x) | ((x) << 16)) & 0x5CA259AA)
#define GET_RX_ADDR_FROM_CHN_PWR(x)			(((x) | ((x) << 16)) & 0xA33D59AA)
static int32_t roamNRF905(uint8_t* pTxBuff, int32_t nTxBuffLen, uint8_t* pRxBuff, int32_t nRxBuffLen) {
	uint8_t nHoppingTimes;
	uint8_t nHoppingIndex;
	int32_t nWaitResult;
	nRF905Mode_t tPreMode = tNRF905Status.tNRF905CurrentMode;
	
	for (nHoppingTimes = 0; nHoppingTimes < MAX_HOPPING_RETRY_TIMES; nHoppingTimes++) {
		for (nHoppingIndex = 0; nHoppingIndex < GET_LENGTH_OF_ARRAY(unCAR_REMOTE_HOPPING_TAB); nHoppingIndex++) {
			setNRF905Mode(NRF905_MODE_STD_BY);
			tNRF905Status.unNRF905CHN_PWR = GET_CHN_PWR_FAST_CONFIG(unCAR_REMOTE_HOPPING_TAB[nHoppingIndex], NRF905_POWER);
			tNRF905Status.unNRF905HoppingCNT++;
			tNRF905Status.unNRF905TxAddr = GET_TX_ADDR_FROM_CHN_PWR(tNRF905Status.unNRF905CHN_PWR);
			tNRF905Status.unNRF905RxAddr = GET_RX_ADDR_FROM_CHN_PWR(tNRF905Status.unNRF905CHN_PWR);
			writeFastConfig(tNRF905Status.unNRF905CHN_PWR);
			writeTxAddr(tNRF905Status.unNRF905TxAddr);
			writeRxAddr(tNRF905Status.unNRF905RxAddr);
			writeTxPayload(pTxBuff, nTxBuffLen);
			setNRF905Mode(NRF905_MODE_BURST_TX);
			tNRF905Status.unNRF905SendFrameCNT++;
			// Timeout or transmit done, I don't care
			osDelay(2);
			setNRF905Mode(NRF905_MODE_BURST_RX);
			nWaitResult = osSemaphoreWait( nRF905SPIDMACpltHandle, 50 );
			if( nWaitResult == osOK ) {
				/* Something received. */
				readRxPayload(pRxBuff, nRxBuffLen);
				setNRF905Mode(tPreMode);
				tNRF905Status.unNRF905RecvFrameCNT++;
				return 0;
			} else {
				
				// Continue retry
			}
		}
	}
	setNRF905Mode(tPreMode);
	return (-1);
}

int32_t nRF905SendFrame(uint8_t* pTxBuff, int32_t nTxBuffLen, uint8_t* pRxBuff, int32_t nRxBuffLen) {
	int32_t nWaitResult;
	nRF905Mode_t tPreMode;
	int32_t nResult;
	
	if (osMutexWait(nRF905OccupyHandle, 500) != osOK) {
		return (-1);
	}
	tPreMode = tNRF905Status.tNRF905CurrentMode;
	// For test only, fix channel, TX and RX address, no hopping
//	writeTxAddr(TEST_NRF905_TX_ADDR);
//	writeRxAddr(TEST_NRF905_RX_ADDR);
	
	writeTxPayload(pTxBuff, nTxBuffLen);
	setNRF905Mode(NRF905_MODE_BURST_TX);
	tNRF905Status.unNRF905SendFrameCNT++;
	// Timeout or transmit done, I don't care
	osDelay(2);
	
	setNRF905Mode(NRF905_MODE_BURST_RX);
	nWaitResult = osSemaphoreWait( DataReadySetHandle, 80 );
	if( nWaitResult == osOK ) {
		/* Something received. */
		readRxPayload(pRxBuff, nRxBuffLen);
		setNRF905Mode(tPreMode);
		tNRF905Status.unNRF905RecvFrameCNT++;
		osMutexRelease(nRF905OccupyHandle);
		return 0;
	} else {
		/* The call to ulTaskNotifyTake() timed out. */
		nResult = roamNRF905(pTxBuff, nTxBuffLen, pRxBuff, nRxBuffLen);
		setNRF905Mode(tPreMode);
		osMutexRelease(nRF905OccupyHandle);
		return nResult;
	}
}
uint32_t unSysTickTest;
void queryCarStatus(void const * argument) {
	uint8_t unCmd[5]; 
	uint8_t unReadFrame[sizeof(CarStatus_t)];
	uint32_t unSysTick;
	unCmd[0] = NRF905_CMD_GET_STATUS; 
	unSysTick = HAL_GetTick();
	memcpy(unCmd + 1, &unSysTick, sizeof(unSysTick)); 
	if (nRF905SendFrame(unCmd, sizeof(unCmd), unReadFrame, sizeof(unReadFrame)) == 0) {
		unSysTickTest = HAL_GetTick() - unSysTick;
		tRemoteCarStatus = *((CarStatus_t*)unReadFrame);
	} 
}

CarStatus_t getCarStatus(void) {
	return tRemoteCarStatus;
}

//uint8_t unReadConfBuff[6];
static int32_t nRF905Initial(void) {
	HAL_GPIO_WritePin(NRF905_CSN_GPIO_Port, NRF905_CSN_Pin, GPIO_PIN_SET);
	setNRF905Mode(NRF905_MODE_STD_BY);
	osSemaphoreWait( nRF905SPIDMACpltHandle, 5 );
	osSemaphoreWait( DataReadySetHandle, 5 );
	osDelay(10);
	nRF905CRInitial();
//	readConfig(0, unReadConfBuff, GET_LENGTH_OF_ARRAY(unReadConfBuff));
	return 0;
}

void startNRF905Trans(void const * argument) {
	nRF905Initial();
	osTimerStart(nCarStatusHandle, 200);
	while (1) {
		osDelay(1000);
		HAL_GPIO_TogglePin(LED2_GPIO_Port, LED2_Pin);
	}
}
