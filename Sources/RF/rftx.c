#include "rftx.h"
#include "hamming1511.h"
#include "cb.h"
#include "timers.h"
#include <stdio.h>

#define RFTX_DATA GLUE(PTT_PTT,RFTX_DATA_TIMER)
#define RFTX_DATA_DDR GLUE(DDRT_DDRT,RFTX_DATA_TIMER)

//f = 3khz
#define RFTX_1_HIGH_TIME_US 130
#define RFTX_1_LOW_TIME_US 200
#define RFTX_0_HIGH_TIME_US 200
#define RFTX_0_LOW_TIME_US 130

#define RFTX_DEAD_TIME_US 400
#define RFTX_START_TX_TIME_US 100

#define RFTX_QUEUE_SIZE 8


struct rftx_commData
{
	u8 id;
	u8 *data;
	s8 length;
	rftx_ptr eot;
};

typedef struct rftx_commData rftx_commData;


typedef cbuf rfqueue;

#define RFQUEUE_EMPTY CB_EMPTY
#define RFQUEUE_FULL CB_FULL

rfqueue rfqueue_Create(u8 *mem, u16 len);

#define rfqueue_Status(queue) cb_status(queue)

bool rfqueue_Push(rfqueue *queue, rftx_commData data);

rftx_commData rfqueue_Pop(rfqueue *queue);

u8 rftx_queueMemory[sizeof(rftx_commData)*RFTX_QUEUE_SIZE];


typedef enum
{
	IDLE,
	SENDING,
	WAITING_FOR_DEAD_TIME_TO_END,
} RFTX_STATUS;

struct
{
	bool ecc;
	RFTX_STATUS status;
	rfqueue queue;
	
	rftx_commData currComm;
	u16 currData;
	s8 currDataIndex;
	bool bitHalfSent;
	u8 dataIndex;
} rftx_data;

bool rftx_isInit = _FALSE;

void rftx_TimerCallback(void);

void rftx_CommenceTX (void);

void rftx_FetchNewdata (void);

void rftx_Init (bool ecc)
{
	if (rftx_isInit == _TRUE)
		return;	
	
	rftx_isInit = _TRUE;
	
	rftx_data.ecc = ecc;
	DDRA = 0xFF;
	PORTA_PA1 = 0;
	
	RFTX_DATA_DDR = DDR_OUT;
		
	rftx_data.queue = rfqueue_Create(rftx_queueMemory,RFTX_QUEUE_SIZE);
	
	tim_Init();	
	tim_GetTimer (TIM_OC, rftx_TimerCallback, NULL, RFTX_DATA_TIMER);
	
	rftx_data.status = WAITING_FOR_DEAD_TIME_TO_END;
	tim_SetValue(RFTX_DATA_TIMER, tim_GetGlobalValue() + TIM_US_TO_TICKS(RFTX_DEAD_TIME_US));
	tim_ClearFlag(RFTX_DATA_TIMER);
	tim_DisconnectOutput(RFTX_DATA_TIMER);
	tim_EnableInterrupts(RFTX_DATA_TIMER);
	
	RFTX_DATA = 1;
	
	return;
}

bool rftx_Send(u8 id, u8 *data, u8 length, rftx_ptr eot)
{
		
	if (rftx_data.status == IDLE)
	{		
		putchar('s');
		rftx_data.status = SENDING;
		rftx_data.currComm.id = id & 0x07;
		rftx_data.currComm.data = data;
		rftx_data.currComm.length = (s8) (length & 0x7F) - 1;
		rftx_data.currComm.eot = eot;
	
		rftx_CommenceTX();
		
		return _TRUE;
	}
	else
	{
		if (rfqueue_Status(&rftx_data.queue) == RFQUEUE_FULL)
			return _FALSE;
		else
		{
			rftx_commData requestedComm;
			putchar('q');
			requestedComm.id = id & 0x07;
			requestedComm.data = data;
			requestedComm.length = (s8) (length & 0x7F) - 1;
			requestedComm.eot = eot;
			rfqueue_Push(&rftx_data.queue,requestedComm);
			
			return _TRUE;
		}
	}
}

void rftx_CommenceTX (void)
{
	rftx_data.dataIndex = 0; //No data is being sent yet
	
	rftx_data.currData = (rftx_data.ecc << 10) | (rftx_data.currComm.id << 7) | (rftx_data.currComm.length); 
	hamm_GetParityBits(& rftx_data.currData);
	rftx_data.currDataIndex = 14; //Start of command
	rftx_data.bitHalfSent = _FALSE;
	RFTX_DATA = 0;
	
	tim_SetValue(RFTX_DATA_TIMER, tim_GetGlobalValue() + TIM_US_TO_TICKS(RFTX_START_TX_TIME_US));
	tim_ClearFlag(RFTX_DATA_TIMER);
	tim_EnableInterrupts(RFTX_DATA_TIMER);
	
	
	return;
}

void rftx_TimerCallback(void)
{
	PORTA_PA1 = 1;
	
	if (rftx_data.status == WAITING_FOR_DEAD_TIME_TO_END)
	{
		if (rfqueue_Status(&rftx_data.queue) == RFQUEUE_EMPTY)
		{
			rftx_data.status = IDLE;
			tim_DisableInterrupts(RFTX_DATA_TIMER);
			
		}
		else
		{
			rftx_commData newComm = rfqueue_Pop(&rftx_data.queue);
			rftx_data.status = SENDING;
			rftx_data.currComm.id = newComm.id & 0x07;
			rftx_data.currComm.data = newComm.data;
			rftx_data.currComm.length = (s8) (newComm.length & 0x7F);
			rftx_data.currComm.eot = newComm.eot;
		
			rftx_CommenceTX();
			
		}
	}
	else // SENDING
	{
		if (rftx_data.bitHalfSent == _TRUE)
		{
			RFTX_DATA = 0;
		
			if ((rftx_data.currData & BIT(rftx_data.currDataIndex)) == 0)
				tim_SetValue(RFTX_DATA_TIMER, tim_GetValue(RFTX_DATA_TIMER) + TIM_US_TO_TICKS(RFTX_0_LOW_TIME_US));
			else
				tim_SetValue(RFTX_DATA_TIMER, tim_GetValue(RFTX_DATA_TIMER) + TIM_US_TO_TICKS(RFTX_1_LOW_TIME_US));
			
			rftx_data.bitHalfSent = _FALSE;
			rftx_data.currDataIndex--;
			
		}
		else
		{
			if (rftx_data.currDataIndex >= 0) // If currData has not been fully transmitted
			{
				RFTX_DATA = 1;
			
				if ((rftx_data.currData & BIT(rftx_data.currDataIndex)) == 0)
					tim_SetValue(RFTX_DATA_TIMER, tim_GetValue(RFTX_DATA_TIMER) + TIM_US_TO_TICKS(RFTX_0_HIGH_TIME_US));
				else
					tim_SetValue(RFTX_DATA_TIMER, tim_GetValue(RFTX_DATA_TIMER) + TIM_US_TO_TICKS(RFTX_1_HIGH_TIME_US));
				
				rftx_data.bitHalfSent = _TRUE;
				
			}
			else
			{
				if (rftx_data.dataIndex > rftx_data.currComm.length) // If all data has been transmitted
				{
					RFTX_DATA = 1;
					
					rftx_data.status = WAITING_FOR_DEAD_TIME_TO_END;
					tim_SetValue(RFTX_DATA_TIMER, tim_GetValue(RFTX_DATA_TIMER) + TIM_US_TO_TICKS(RFTX_DEAD_TIME_US));
					rftx_data.currComm.eot();
				}
				else // Fetch new data
				{
					RFTX_DATA = 1;

					rftx_FetchNewData();
					
					if ((rftx_data.currData & BIT(rftx_data.currDataIndex)) == 0)
						tim_SetValue(RFTX_DATA_TIMER, tim_GetValue(RFTX_DATA_TIMER) + TIM_US_TO_TICKS(RFTX_0_HIGH_TIME_US));
					else
						tim_SetValue(RFTX_DATA_TIMER, tim_GetValue(RFTX_DATA_TIMER) + TIM_US_TO_TICKS(RFTX_1_HIGH_TIME_US));

					rftx_data.bitHalfSent = _TRUE;
				}
				
				
			}
		}
	}
	PORTA_PA1 = 0;
}

void rftx_FetchNewData (void)
{
	s8 bitIndex; 
	u8 firstByte; 
	u8 bitsToSend;					

	if ((rftx_data.currComm.length - rftx_data.dataIndex) < 10)
		bitsToSend = rftx_data.currComm.length - rftx_data.dataIndex + 1;
	else
		bitsToSend = 11;

	bitIndex = rftx_data.dataIndex % 8;
	firstByte = rftx_data.dataIndex / 8;

	// First byte
	rftx_data.currData = ((u16) (rftx_data.currComm.data[firstByte] & firstBits(8 - bitIndex))) << (3 + bitIndex);

	// Second byte
	if (bitIndex < 5) // If bitIndex == 5, 3 bits were read from the first byte, and shift is 0
		rftx_data.currData += ((u16) (rftx_data.currComm.data[firstByte + 1] & lastBits(3 + bitIndex))) << (bitIndex - 5);
	else
		rftx_data.currData += ((u16) (rftx_data.currComm.data[firstByte + 1] & lastBits(3 + bitIndex))) >> (5 - bitIndex);

	// Third byte					
	if (bitIndex == 6)
		rftx_data.currData += ((u16) (rftx_data.currComm.data[firstByte + 2] & lastBits(1))) >> 7;
	else if (bitIndex == 7)
		rftx_data.currData += ((u16) (rftx_data.currComm.data[firstByte + 2] & lastBits(2))) >> 6;
						
	// Fix: si bitsToSend < 11, tengo que sacar un par de las que lei. Problema: lei uno o dos bytes de más

	rftx_data.dataIndex = rftx_data.dataIndex + 11;
	if (rftx_data.ecc)
	{
		hamm_GetParityBits(& rftx_data.currData);
		rftx_data.currDataIndex = 14; //Start of data + parity bits
	}
	else
		rftx_data.currDataIndex = 10; //Start of data	

	return;
}


rfqueue rfqueue_Create(u8 *mem, u16 len)
{
	return cb_create(mem, sizeof(rftx_commData)*len);
}

bool rfqueue_Push(rfqueue* queue, rftx_commData data)
{
	if (rfqueue_Status(queue) == RFQUEUE_FULL)
		return _FALSE;
	else
	{
		cb_push(queue, data.id);
		cb_push(queue, (u8) data.data);
		cb_push(queue, (u8) (((u16)data.data) >> 8));		
		cb_push(queue, (u8) data.eot);
		cb_push(queue, (u8) (((u16)data.eot) >> 8));		
		cb_push(queue, data.length);
		
		return _TRUE;
	}
}

rftx_commData rfqueue_Pop(rfqueue* queue)
{
	rftx_commData read;
	
	if (rfqueue_Status(queue) == RFQUEUE_EMPTY)
	{
		read.id = 0;
		read.data = NULL;
		read.eot = NULL;
		read.length = 0;
	}
	else
	{
		read.id = cb_pop(queue);
		read.data = (u8 *) cb_pop(queue);
		read.data = (u8 *) (((u16) read.data + (((u16)cb_pop(queue)) << 8))); 
		read.eot = (u8 *) cb_pop(queue);
		read.eot = (u8 *) (((u16) read.eot + (((u16)cb_pop(queue)) << 8))); 
		read.length = (s8) cb_pop(queue);
	}
	
	return read;
}
