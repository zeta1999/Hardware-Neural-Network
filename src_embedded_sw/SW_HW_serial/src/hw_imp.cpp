#include "hw_imp.hpp"

/* Device hardware build related constants. */
#define DMA_DEV_ID        		XPAR_AXIDMA_0_DEVICE_ID
#define DDR_BASE_ADDR			XPAR_DDR_MEM_BASEADDR
#define MEM_BASE_ADDR			(DDR_BASE_ADDR + 0x1000000)

/* AXIS buffers */
#define TX_BUFFER_BASE			(MEM_BASE_ADDR + 0x00100000)
#define RX_BUFFER_BASE			(MEM_BASE_ADDR + 0x00300000)
#define RX_BUFFER_HIGH			(MEM_BASE_ADDR + 0x004FFFFF)
// output buffer has size: 0x300000 - 0x100000 = 0x200000

#define LABELS_PER_TRANSFER		8

#define NEW_TASK 	(1 << 19)
#define SAME_TASK 	(~(1 << 19))
#define CONTINUE	(1 << 18)
#define STOP		(~(1 << 18))

int config_dma();
u32 init_opcode(u16 nb_ex);
u16 init_tx_buffer(u8* features, u16 nb_ex, u8* labels = NULL);

XAxiDma AxiDma;
u32* TxBufferPtr = (u32 *)TX_BUFFER_BASE;
u32* RxBufferPtr = (u32 *)RX_BUFFER_BASE;

session_t tsk;
option_t opt = PREV;
u16 *weights_ptr = NULL;

void hw_predict(u8* features, u8* results, u16 nb_ex)
{
	XTime t_start, t_end;
	xil_printf("[INFO] Format data.\n");
	tsk = PREDICTION;
	TxBufferPtr[0] = init_opcode(nb_ex) | NEW_TASK;

	u16 nb_data_words = init_tx_buffer(features, nb_ex);
	u16 nb_pred_words = ceil_div(nb_ex,LABELS_PER_TRANSFER);

	xil_printf("[INFO] Configure transfer with HW.\n");
	config_dma();

	XTime_GetTime(&t_start);
	Xil_DCacheFlushRange((u32)TxBufferPtr, nb_data_words*4);
	XAxiDma_SimpleTransfer(&AxiDma,(u32) (RxBufferPtr),
			nb_pred_words*4, XAXIDMA_DEVICE_TO_DMA);
	XAxiDma_SimpleTransfer(&AxiDma,(u32) (TxBufferPtr),
			nb_data_words*4, XAXIDMA_DMA_TO_DEVICE);

	while (XAxiDma_Busy(&AxiDma,XAXIDMA_DMA_TO_DEVICE));
	while (XAxiDma_Busy(&AxiDma,XAXIDMA_DEVICE_TO_DMA));

	XTime_GetTime(&t_end);
	u32 delay = 1.0 * (t_end - t_start) / (COUNTS_PER_SECOND / 1000000); // us
	xil_printf("[INFO] Duration of prediction: %u us\n", delay);

	xil_printf("[INFO] Result received.\n");

	Xil_DCacheInvalidateRange((u32)RxBufferPtr, nb_pred_words*4);
	u16 i,j;
	for (i = 0; i < (nb_ex/LABELS_PER_TRANSFER); i++ ) {
		for (j = 0; j < LABELS_PER_TRANSFER; j++) {
			results[i*LABELS_PER_TRANSFER + j] = (RxBufferPtr[i] >> (4*j)) & 0xF;
		}
	}
	for (i = 0; i < (nb_ex % LABELS_PER_TRANSFER); i++) {
		results[(nb_ex/LABELS_PER_TRANSFER)*LABELS_PER_TRANSFER + i] = (RxBufferPtr[(nb_ex/LABELS_PER_TRANSFER)] >> (4*i)) & 0xF;
	}

	opt = PREV;
	return;
}

float hw_train(u8* features, u8* labels, u16 nb_ex, u16 nb_iter)
{
	xil_printf("[INFO] Format data.\n");
	tsk = TRAINING;
	u32 opcode_new = init_opcode(nb_ex) | NEW_TASK;
	u32 opcode_continue = (CONTINUE) | (nb_ex & 0xFFFF) & (SAME_TASK);
	u32 opcode_stop = STOP & (nb_ex & 0xFFFF) & SAME_TASK;
	xil_printf("Stop opcode: %x", opcode_stop);
	u16 nb_data_words = init_tx_buffer(features, nb_ex, labels);
	u16 nb_resp_words = 1;
	u16 cost;

	TxBufferPtr[0] = opcode_new;

	xil_printf("[INFO] Configure transfer with HW.\n");
	config_dma();

	xil_printf("[INFO] Start training.\n");
	u16 i;
	for(i = 0; i < nb_iter; i++) {
		Xil_DCacheFlushRange((u32)TxBufferPtr, nb_data_words*4);
		XAxiDma_SimpleTransfer(&AxiDma,(u32) (RxBufferPtr),
				nb_resp_words*4, XAXIDMA_DEVICE_TO_DMA);
		XAxiDma_SimpleTransfer(&AxiDma,(u32) (TxBufferPtr),
				nb_data_words*4, XAXIDMA_DMA_TO_DEVICE);

		while (XAxiDma_Busy(&AxiDma,XAXIDMA_DMA_TO_DEVICE));
		while (XAxiDma_Busy(&AxiDma,XAXIDMA_DEVICE_TO_DMA));

		Xil_DCacheInvalidateRange((u32)RxBufferPtr, nb_resp_words*4);
		cost = RxBufferPtr[0] & 0xFFFF;
		xil_printf("Iteration %u \t\tCost: %u\n",i,cost);
		TxBufferPtr[0] = opcode_continue;
	}

	TxBufferPtr[0] = opcode_stop;
	Xil_DCacheFlushRange((u32)TxBufferPtr, nb_data_words*4);
	XAxiDma_SimpleTransfer(&AxiDma,(u32) (TxBufferPtr),
			1*4, XAXIDMA_DMA_TO_DEVICE);
	while (XAxiDma_Busy(&AxiDma,XAXIDMA_DMA_TO_DEVICE));

	opt = PREV;
	return 0; // maybe return the accuracy ?
}

void hw_load_weights(u16* weights)
{
	 opt = GIVEN;
	 weights_ptr = weights;
}

void hw_rand_weights()
{
	opt = RAND;
}

int config_dma()
{
	u16 DeviceId = DMA_DEV_ID;
	int Status;

	XAxiDma_Config *CfgPtr;
	CfgPtr = XAxiDma_LookupConfig(DeviceId);

	if (!CfgPtr) {
		xil_printf("No config found for %d\r\n", DeviceId);
		return XST_FAILURE;
	}
	Status = XAxiDma_CfgInitialize(&AxiDma, CfgPtr);
	if (Status != XST_SUCCESS) {
		xil_printf("Initialization failed %d\r\n", Status);
		return XST_FAILURE;
	}

	if(XAxiDma_HasSg(&AxiDma)){
		xil_printf("Device configured as SG mode \r\n");
		return XST_FAILURE;
	}

	/* Disable interrupts, we use polling mode */
	XAxiDma_IntrDisable(&AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
	XAxiDma_IntrDisable(&AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DMA_TO_DEVICE);

	return XST_SUCCESS;
}

u32 init_opcode(u16 nb_ex)
{
	u8 tmp;
	switch(opt) {
	case GIVEN:
		tmp = 0;
		break;
	case PREV:
		tmp = 2;
		break;
	case RAND:
	default:
		tmp = 1;
	}

	u32 code = ((tsk == TRAINING) << 18)
				| ((tmp & 0b11) << 16)
				| (nb_ex & 0xFFFF);
	return code;
}


u16 init_tx_buffer(u8* features, u16 nb_ex, u8* labels)
{
	u32 i, j;
	u16 offset = 0;

	if (opt == GIVEN)
	{
		u16 *weights1 = weights_ptr;
		u16 *weights2 = weights_ptr + ceil_div(NB_WEIGHTS1,2);

		for(i = 0; i <  (NB_WEIGHTS1/2); i++) {
			TxBufferPtr[1 + i] = ( (weights1[i*2 + 1] & 0xFFF) << 12)
								   | (weights1[i*2] & 0xFFF);
		}
		if ((NB_WEIGHTS1 % 2) == 1) {
			TxBufferPtr[1 + (NB_WEIGHTS1/2)] = weights1[NB_WEIGHTS1 - 1];
		}
		for(i = 0; i <  (NB_WEIGHTS2/2); i++) {
			TxBufferPtr[1 + ceil_div(NB_WEIGHTS1,2) + i] = ( (weights2[i*2 + 1] & 0xFFF) << 12)
															 | (weights2[i*2] & 0xFFF);
		}
		if ((NB_WEIGHTS2 % 2) == 1) {
			TxBufferPtr[1 + ceil_div(NB_WEIGHTS1,2) + (NB_WEIGHTS2/2)] = weights2[NB_WEIGHTS2 - 1];
		}

		offset = ceil_div(NB_WEIGHTS1,2) + ceil_div(NB_WEIGHTS2,2);
	}

	for(i = 0; i < nb_ex; i++) {
		for(j = 0; j < (int)(NB_FEATURES/4); j++) {
			TxBufferPtr[1 + offset + i*ceil_div(NB_FEATURES+1,4) + j]
						= ( features[i*NB_FEATURES + j*4 + 3] << 24 )
						| ( features[i*NB_FEATURES + j*4 + 2] << 16 )
						| ( features[i*NB_FEATURES + j*4 + 1] << 8 )
						|   features[i*NB_FEATURES + j*4 + 0];
		}
		unsigned tmp = 1 + offset + i*ceil_div(NB_FEATURES+1,4) + (NB_FEATURES/4);
		TxBufferPtr[tmp] = 0;
		for(j = 0; j < (NB_FEATURES % 4); j++) {
			TxBufferPtr[tmp] |= (features[i*NB_FEATURES + (NB_FEATURES/4)*4 + j] - 1) << j*8;
		}
		if(labels) {
			TxBufferPtr[tmp] |= labels[i] << (NB_FEATURES % 4)*8;
		}
	}

	return (1 + offset + nb_ex*ceil_div(NB_FEATURES+1,4));
}
