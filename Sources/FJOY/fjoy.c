#include "fjoy.h"
#include "rti.h"
#include "error.h"
#include <stdio.h>

#define FJOY_READ_CHANNEL(chann, cb) atd_SetTask(FJOY_ATD_MODULE, chann, FJOY_ATD_OVERSAMPLING, _FALSE, _FALSE, cb)

void fjoy_UpdateStatus (void *data, rti_time period, rti_id id);
void fjoy_ATDCallback (s16* mem, const struct atd_task* taskData);

#define FJOY_ATD_FIRST_CHANN FJOY_YAW_CHANN

#define LINEAR_SCALE_U8(x,min,max) (((x-min)*255)/(max-min))
#define LINEAR_SCALE_S8(x,min,max,repos) ((x > repos) ? LINEAR_SCALE_S8_UPP(x,repos,max) : LINEAR_SCALE_S8_LOW(x,repos,min))
#define LINEAR_SCALE_S8_UPP(x,repos,max) (((x-repos)*127)/(max-repos))
#define LINEAR_SCALE_S8_LOW(x,repos,min) (((x-repos)*(-128))/(min-repos))


#define SATURATE_U8(x) ((x > 255) ? 255 : ((x < 0) ? 0 : x))
#define SATURATE_S8(x) ((x > 127) ? 127 : ((x < -128) ? -128 : x))


// Calibration

#define YAW_MIN 78
#define YAW_MAX 117
#define YAW_REST 106

#define PITCH_MIN -81
#define PITCH_MAX 92
#define PITCH_REST 0

#define ROLL_MIN 82
#define ROLL_MAX 121
#define ROLL_REST 114

#define ELEV_MIN 78
#define ELEV_MAX 221


struct {
	fjoy_callback callback[FJOY_MAX_CALLBACKS];
	s32 yawSum;
	s32 pitchSum;
	s32 rollSum;
	s32 elevSum;
	bool axesRead;
} fjoy_data;

bool fjoy_isInit = _FALSE;


void fjoy_Init(void)
{
	u8 i;
	
	if (fjoy_isInit == _TRUE)
		return;
	
	
	for (i = 0; i < FJOY_MAX_CALLBACKS; i++)
		fjoy_data.callback[i] = NULL;

	rti_Init();	
	//rti_Register(fjoy_UpdateStatus, NULL, RTI_MS_TO_TICKS(FJOY_SAMPLE_PERIOD_MS), RTI_NOW); BUTTON SMAPLING FUNCT
	
	atd_Init(FJOY_ATD_MODULE);
	fjoy_data.axesRead = _FALSE;
	FJOY_READ_CHANNEL(FJOY_ATD_FIRST_CHANN, fjoy_ATDCallback);

	while (fjoy_data.axesRead == _FALSE) // && !buttons_read
		;
	
	rti_Register(fjoy_UpdateStatus, NULL, RTI_MS_TO_TICKS(FJOY_SAMPLE_PERIOD_MS), RTI_NOW);
	
	return;
}

void fjoy_CallOnUpdate(fjoy_callback cb)
{
	u8 i;
	
	if (cb == NULL)
		err_Throw("fjoy: attempt to register a NULL callback.\n");
	
	for (i = 0; i < FJOY_MAX_CALLBACKS; i++)
		if 	(fjoy_data.callback[i] == NULL)
			break;
	
	if (i == FJOY_MAX_CALLBACKS)
		err_Throw("fjoy: attempt to register more callbacks than there's memory for.\n");
	else
		fjoy_data.callback[i] = cb;
	
	return;
}

void fjoy_ATDCallback (s16* mem, const atd_task* taskData)
{
	u8 i;

	switch (taskData->channel)
	{
		case FJOY_YAW_CHANN:
		
			fjoy_data.yawSum = 0;
			for (i = 0; i < FJOY_ATD_OVERSAMPLING; i++)
				fjoy_data.yawSum += mem[i];
				
			FJOY_READ_CHANNEL(FJOY_PITCH_CHANN, fjoy_ATDCallback);
			
			break;
		case FJOY_PITCH_CHANN:
		
			fjoy_data.pitchSum = 0;
			for (i = 0; i < FJOY_ATD_OVERSAMPLING; i++)
				fjoy_data.pitchSum += mem[i];
				
			FJOY_READ_CHANNEL(FJOY_ROLL_CHANN, fjoy_ATDCallback);
			
			break;
		case FJOY_ROLL_CHANN:
		
			fjoy_data.rollSum = 0;
			for (i = 0; i < FJOY_ATD_OVERSAMPLING; i++)
				fjoy_data.rollSum += mem[i];
				
			FJOY_READ_CHANNEL(FJOY_ELEV_CHANN, fjoy_ATDCallback);
			
			break;
		case FJOY_ELEV_CHANN:
		
			fjoy_data.elevSum = 0;
			for (i = 0; i < FJOY_ATD_OVERSAMPLING; i++)
				fjoy_data.elevSum += mem[i];
				
			fjoy_data.axesRead = _TRUE;
			
			break;
	}
}

void fjoy_UpdateStatus (void *data, rti_time period, rti_id id)
{
	u8 i;
	
	if (fjoy_data.axesRead == _FALSE)
		err_Throw("fjoy: axes sampling frequency is too low.\n");
	
	// do stuff (average samples of buttons and analog inputs, scale inputs)
	
	
	// Division by 4 is required since the inputs are sampled at 10 bits resolution (which must be lowered to 8 bits)
	// Yaw, pitch and roll are s8, substracting 128 makes them go from -128 to 127
	fjoy_data.yawSum = fjoy_data.yawSum / (FJOY_ATD_OVERSAMPLING * 4) - 128;
	fjoy_data.pitchSum = fjoy_data.pitchSum / (FJOY_ATD_OVERSAMPLING * 4) - 128;
	fjoy_data.rollSum = fjoy_data.rollSum / (FJOY_ATD_OVERSAMPLING * 4) - 128;
	// Elevation potentiometer is u8, but the potentiometer is inverted, substracting the measurement from 255 fixes that
	fjoy_data.elevSum = 255 - fjoy_data.elevSum / (FJOY_ATD_OVERSAMPLING * 4); 
	
	// Scaling and saturation
	fjoy_data.yawSum = LINEAR_SCALE_S8(fjoy_data.yawSum, YAW_MIN, YAW_MAX, YAW_REST);
	fjoy_status.yaw = SATURATE_S8(fjoy_data.yawSum);

	fjoy_data.rollSum = LINEAR_SCALE_S8(fjoy_data.rollSum, ROLL_MIN, ROLL_MAX, ROLL_REST);
	fjoy_status.roll = SATURATE_S8(fjoy_data.rollSum);
	
	fjoy_data.pitchSum = LINEAR_SCALE_S8(fjoy_data.pitchSum, PITCH_MIN, PITCH_MAX, PITCH_REST);
	fjoy_status.pitch = SATURATE_S8(fjoy_data.pitchSum);
	
	fjoy_data.elevSum = LINEAR_SCALE_U8(fjoy_data.elevSum, ELEV_MIN, ELEV_MAX);
	fjoy_status.elev = SATURATE_U8(fjoy_data.elevSum);
	

	for (i = 0; i < FJOY_MAX_CALLBACKS; i++)
		if (fjoy_data.callback[i] != NULL)
			(*fjoy_data.callback[i]) ();
		
	fjoy_data.axesRead = _FALSE;
	FJOY_READ_CHANNEL(FJOY_ATD_FIRST_CHANN, fjoy_ATDCallback);
	
	return;
}


