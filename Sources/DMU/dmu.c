#include "dmu.h"
#include <stdio.h>
#include "quick_serial.h"

#define PRINT_START 1
#define PRINT_LENGTH (ADD_WHO_AM_I - PRINT_START)+1

#define MAX_BURST_READS 256
#define INITIAL_AVERAGE 256

void dmu_printI2CData(void);

extern void printI2CData(void);


// Measurements taken individually must respect MPU internal registers' order
struct dmu_measurements_T
{
	s16 accel_x;
	s16 accel_y;
	s16 accel_z;
	
	s16 temp;
	
	s16 gyro_x;
	s16 gyro_y;
	s16 gyro_z;

} dmu_measurements;

// For samples taken from fifo buffer
struct dmu_samples_T
{
	s16 accel_x;
	s16 accel_y;
	s16 accel_z;
	
	s16 gyro_x;
	s16 gyro_y;
	s16 gyro_z;
};


struct dmu_sampleAccumulator_T
{
	s32 accel_x;
	s32 accel_y;
	s32 accel_z;
	
	s32 gyro_x;
	s32 gyro_y;
	s32 gyro_z;
	
	u16 numberOfSamples;
	
}dmu_sampleAccumulator = {0, 0, 0, 0, 0, 0, 0};


struct dmu_gyroOffset_T
{
	s16 x;
	s16 y;
	s16 z;

}dmu_gyroOffset;

struct dmu_data_T dmu_data = {_FALSE, NULL, 0, {_TRUE, 0, 0, 0, NULL} };


void dmu_PrintFormattedMeasurements(void);
void dmu_FifoStageRead(void);
void dmu_CommFailed(void);
void dmu_printFifoCnt(void);
void dmu_AccumulateSamples(struct dmu_sampleAccumulator_T* acc, struct dmu_samples_T* samples);
void dmu_AccumulateMeasurements(struct dmu_sampleAccumulator_T* acc, struct dmu_measurements_T* measurements);
void dmu_DivideAccumulator(struct dmu_sampleAccumulator_T* acc);
void dmu_AverageSamples(void);
void dmu_StagesInit(void);	
void dmu_cleanAccumulator(struct dmu_sampleAccumulator_T* acc);
void dmu_FifoAverageInit(void);
void dmu_GetAndAccMeasurements(void);
void dmu_AccumulateMeasurementWrapper(void);


// Reset macro - g, a, t are booleans (1/0), cb is callback after reset is done..
#define dmu_SignalReset(g, a, t, cb) do {			\
	iic_commData.data[0] = ADD_SIGNAL_PATH_RESET;	\
	iic_commData.data[1] = RESET_SIGNAL(g,a,t);		\
	dmu_Send(cb, NULL, 2, NULL);					\
} while(0)

// Reg 114	-	Gets FIFO cnt and stores it in I2C buffer, calling 'cb' when done.
#define dmu_GetFifoCount(cb) 										\
	(dmu_ReceiveFromRegister (ADD_FIFO_CNT_H, cb, NULL, 2, NULL))
// Reg 116
#define dmu_ReadNFifoBytes(_n, cb)	\
	dmu_ReceiveFromRegister (ADD_FIFO_RW, cb, NULL, _n, NULL)



void dmu_Init()
{
	if (dmu_data.init == _TRUE)
		return;
	
	iic_Init();
	
	
	switch (dmu_data.stage)
	{u16 i;

	case 0:
			
		iic_commData.data[0] = ADD_PWR_MGMT_1;
		iic_commData.data[1] = PWR_MGMT_1_RESET;

		dmu_Send (dmu_Init, dmu_CommFailed, 2, NULL);
	
		dmu_data.stage++;
		
		break;

	case 1:
		// Note: inserting delay / putchars here screws configuration up.	
				
		iic_commData.data[0] = ADD_SAMPLE_RATE_DIVIDER;
		iic_commData.data[1] = SAMPLE_RATE_DIVIDER;	// 25
		iic_commData.data[2] = CONFIG;				// 26
		iic_commData.data[3] = GYRO_CONFIG(GYRO_X_SELFTEST, GYRO_Y_SELFTEST, GYRO_Z_SELFTEST);		// 27
		iic_commData.data[4] = ACCEL_CONFIG(ACCEL_X_SELFTEST, ACCEL_Y_SELFTEST, ACCEL_Z_SELFTEST);	//28
		iic_commData.data[5] = FREE_FALL_THRESHOLD;
		iic_commData.data[6] = FREE_FALL_DURATION;
		iic_commData.data[7] = MOTION_INT_THRESHOLD;
		iic_commData.data[8] = MOTION_INT_DURATION;
		iic_commData.data[9] = ZERO_MOTION_THRESHOLD;
		iic_commData.data[10] = ZERO_MOTION_DURATION;
		iic_commData.data[11] = FIFO_ENABLE;
		
		dmu_Send (dmu_Init, dmu_CommFailed, 12, NULL);
		putchar('a');
		dmu_data.stage++;
		
		break;
	
	case 2:
		putchar('b');
	
		iic_commData.data[0] = ADD_INT_PIN_CFG;
		iic_commData.data[1] = INT_PIN_CFG;		// 55
		iic_commData.data[2] = INT_ENABLE;

		dmu_Send(dmu_Init, dmu_CommFailed, 3, NULL);
		
		dmu_data.stage++;
		
		break;
		
	case 3:

		iic_commData.data[0] = ADD_SIGNAL_PATH_RESET;
		iic_commData.data[1] = RESET_SIGNAL(1,1,1);
		iic_commData.data[2] = MOTION_DETECT_CTRL;
		iic_commData.data[3] = USER_CTRL(0,1,1);	// Run means not reset.
		iic_commData.data[4] = PWR_MGMT_1_RUN;
		// PWR_MGMT_2 stays in 0 (reset value).
		
		dmu_Send(dmu_Init, dmu_CommFailed, 5, NULL);

		dmu_data.stage++;

		break;		

	case 4:
/*		for (i = 0; i < INITIAL_AVERAGE; i++)
		{
			putchar(i + '0');
			dmu_GetMeasurements();
			while (i == dmu_sampleAccumulator.numberOfSamples);		// Wait for sample to get accumulated before asking for next sample
		}
		dmu_DivideAccumulator(&dmu_sampleAccumulator);

		dmu_gyroOffset.x = (s16)dmu_sampleAccumulator.gyro_x;
		dmu_gyroOffset.y = (s16)dmu_sampleAccumulator.gyro_y;
		dmu_gyroOffset.z = (s16)dmu_sampleAccumulator.gyro_z;

		printf("ox: %d, oy: %d, oz: %d\n", dmu_gyroOffset.x, dmu_gyroOffset.y, dmu_gyroOffset.z);
*/
		dmu_data.stage++;

		// No break here.	
	case 5:

		iic_commData.data[0] = ADD_USER_CTRL;
		iic_commData.data[1] = USER_CTRL_INIT;	// Run means not reset.

		dmu_Send(dmu_Init, dmu_CommFailed, 2, NULL);

		dmu_data.stage++;

		break;		


	case 6:		// Done for now - No need of resets or pwr mgmt.
				
		dmu_data.init = _TRUE;
		dmu_data.stage = 0;

		break;
		
	default: 
		break;
	}
	
	while (dmu_data.init == _FALSE)
		;

	
	return;
}


void dmu_GetMeasurements(void)
{
	u8* dataPtr = (u8*)&dmu_measurements;
	dmu_ReceiveFromRegister(ADD_ACCEL_OUT, dmu_PrintFormattedMeasurements, NULL, sizeof(dmu_measurements), dataPtr);
}

void dmu_PrintFormattedMeasurements(void)
{
	struct dmu_measurements_T* dm = &dmu_measurements; 
	printf("ax: %d, ay: %d, az: %d\ngx: %d, gy: %d, gz: %d\n", dm->accel_x, dm->accel_y, dm->accel_z, dm->gyro_x, dm->gyro_y, dm->gyro_z);
	return;
}


void dmu_GetAndAccMeasurements(void)
{
	u8* dataPtr = (u8*)&dmu_measurements;
	dmu_ReceiveFromRegister(ADD_ACCEL_OUT, dmu_AccumulateMeasurementWrapper, dmu_CommFailed, sizeof(dmu_measurements), dataPtr);
}

void dmu_AccumulateMeasurementWrapper(void)
{
	printf("hola\n");
	dmu_AccumulateMeasurements(&dmu_sampleAccumulator, &dmu_measurements);
	dmu_sampleAccumulator.numberOfSamples++;
	
	return;
}

	
	
void dmu_FifoStageRead(void)	
{
	switch (dmu_data.stage)
	{
	case 0:
		dmu_GetFifoCount(dmu_FifoStageRead);
		dmu_data.stage++;		
		break;

	case 1:
		// Fifo reads can be 256 bytes max, even though fifo full length is 1024. 
		
		dmu_data.fifo.count = *((u16*)iic_commData.data);

		dmu_data.fifo.fetchTimes = dmu_data.fifo.count / MAX_BURST_READS;
		dmu_data.fifo.remainingBytes = dmu_data.fifo.count % MAX_BURST_READS;		

		dmu_data.stage++;
		// No break here.

	case 2:

		dmu_data.fifo.fetchTimes--;

		printf("fc: %d, ft: %d, rb: %d\n", dmu_data.fifo.count, dmu_data.fifo.fetchTimes, dmu_data.fifo.remainingBytes);
		
		if ((dmu_data.fifo.fetchTimes < 0) && (dmu_data.fifo.remainingBytes != 0))
			dmu_ReadNFifoBytes(dmu_data.fifo.remainingBytes, dmu_data.fifo.stageCb);
		else
			dmu_ReadNFifoBytes(MAX_BURST_READS, dmu_data.fifo.stageCb);
		
		break;

	default:
		break;
	}
}


void dmu_printI2CData(void)
{

	u16 i;
	for (i = 0; i < PRINT_LENGTH; i++)
	{
		printf("%d %x\n", PRINT_START + i, iic_commData.data[i]);
		iic_commData.data[i] = '\0';
	}
}


void dmu_CommFailed()
{
	printf("comm failed, stage %d\n", dmu_data.stage);
	dmu_data.stage = 0;
}


void dmu_PrintFifoMem(void)
{
	u16 i, limit;
	
	limit = dmu_GetIterationLimit() / sizeof(u16);
	
	for (i = 0; i < limit; i++) 
	{
		printf("%d\t", (*(((u16*)iic_commData.data) + i) ) );
	}
		
	dmu_ContinueFifoAction();		
	
	if (dmu_data.userCb != NULL)
		dmu_data.userCb();
	
	return;
}


void dmu_printFifoCnt(void)
{
	printf("fCnt: %d\n", (*(u16*)iic_commData.data));
}


void dmu_GetSamples(void)
{
	dmu_FifoAverage(dmu_AverageSamples);
	return;
}


void dmu_FifoAverageInit(void)
{
	dmu_sampleAccumulator.numberOfSamples = dmu_data.fifo.count / sizeof (struct dmu_samples_T);
	dmu_cleanAccumulator(&dmu_sampleAccumulator);
	
	dmu_data.fifo.stageCb = dmu_AverageSamples;
	dmu_AverageSamples();
	
	return;
}


void dmu_AverageSamples(void)
{
	struct dmu_samples_T* dmuSamples = (struct dmu_samples_T*)iic_commData.data;
	struct dmu_sampleAccumulator_T const * acc = &dmu_sampleAccumulator;
	u16 i;
	u16 limit;
	
	limit = dmu_GetIterationLimit() / sizeof(struct dmu_samples_T);

	for (i = 0; i < limit; i++)
		dmu_AccumulateSamples(acc, dmuSamples++);

	dmu_ContinueFifoAction();
	
	// ContinueFifoAction goes retrieves more data from fifo if there is any and returns. 
	// If it does not return, it means there is no more data left and final division has to be done.
	dmu_DivideAccumulator(acc);
	
	if (dmu_data.userCb != NULL)
		dmu_data.userCb();
	
	return;
}

void dmu_AccumulateSamples(struct dmu_sampleAccumulator_T* acc, struct dmu_samples_T* samples)
{
	acc->accel_x += samples->accel_x;
	acc->accel_y += samples->accel_y;
	acc->accel_z += samples->accel_z;

	acc->gyro_x += samples->gyro_x;
	acc->gyro_y += samples->gyro_y;
	acc->gyro_z += samples->gyro_z;
	
	return;
}


void dmu_AccumulateMeasurements(struct dmu_sampleAccumulator_T* acc, struct dmu_measurements_T* measurements)
{
	acc->accel_x += measurements->accel_x;
	acc->accel_y += measurements->accel_y;
	acc->accel_z += measurements->accel_z;

	acc->gyro_x += measurements->gyro_x;
	acc->gyro_y += measurements->gyro_y;
	acc->gyro_z += measurements->gyro_z;
	
	return;
}


void dmu_DivideAccumulator(struct dmu_sampleAccumulator_T* acc)
{
	acc->accel_x /= acc->numberOfSamples;
	acc->accel_y /= acc->numberOfSamples;
	acc->accel_z /= acc->numberOfSamples;

	acc->gyro_x /= acc->numberOfSamples;
	acc->gyro_y /= acc->numberOfSamples;
	acc->gyro_z /= acc->numberOfSamples;
	
	return;
}


void dmu_cleanAccumulator(struct dmu_sampleAccumulator_T* acc)
{
	acc->accel_x = 0;
	acc->accel_y = 0;
	acc->accel_z = 0;

	acc->gyro_x = 0;
	acc->gyro_y = 0;
	acc->gyro_z = 0;
}


