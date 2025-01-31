#include <stdio.h>
#include <stdlib.h>
#include "xparameters.h"
#include "xil_printf.h"
#include "xtime_l.h"

#include "utility.hpp"
#include "sw_imp.hpp"
#include "hw_imp.hpp"

void get_opcode();
void get_data();
void send_predictions();

enum implementation_t {SOFTWARE, HARDWARE};
enum state_t {Get_opcode, Get_data, Process, Send_result};
implementation_t implementation;
state_t state = Get_opcode;

session_t session;
option_t option;

u32 opcode = 0;
u16 nb_examples = 0, nb_iterations = 0;

u8 *features, *labels, *predictions = NULL;
u16 *weights = NULL;

int main()
{
	XTime t_start, t_end;

	while(1)
	{
		get_opcode();
		get_data();

		// Launch timer
		XTime_GetTime(&t_start);

		if (implementation == SOFTWARE) {
			if (option == GIVEN) {
				sw_load_weights(weights);
			}
			else if(option == RAND) {
				sw_rand_weights();
			}
			if (session == TRAINING) {
				xil_printf("[INFO] Starting training.\n");
				float accuracy = sw_train(features, labels, nb_examples, nb_iterations);
				xil_printf("Training accuracy: %u %% \n", u32(accuracy));
			}
			else {
				sw_predict(features, predictions, nb_examples);
			}
		}
		else {
			if (option == GIVEN) {
				hw_load_weights(weights);
			}
			else if(option == RAND) {
				hw_rand_weights();
			}
			if (session == TRAINING) {
				xil_printf("[INFO] Starting training.\n");
				float accuracy = hw_train(features, labels, nb_examples, nb_iterations);
			}
			else {
				hw_predict(features, predictions, nb_examples);
			}
		}

		// Stop timer and send print result
		XTime_GetTime(&t_end);
		u32 delay = 1.0 * (t_end - t_start) / (COUNTS_PER_SECOND / 1000); // ms
		xil_printf("[INFO] Duration of task: %u ms\n", delay);

		send_code(c_DONE);

		if (session == TRAINING) {

		}
		else {
			send_predictions();
			//xil_printf("Results sent..\n");
		}

		delete[] features; features = NULL;
		delete[] weights; weights = NULL;
		delete[] labels; labels = NULL;
		delete[] predictions; predictions = NULL;

		send_code(c_END);
	}
	return 0;
}

void get_opcode()
{
	u32 i;
	opcode = 0;
	for(i = 0; i < 4; i++) {
		opcode |= u8(inbyte()) << i*8;
	}
	outbyte(c_ACK);

	switch((opcode >> 19) & 1) {
	case 0:
		implementation = SOFTWARE;
		break;
	default:
		implementation = HARDWARE;
		break;
	}

	switch((opcode >> 18) & 1) {
	case 0:
		session = TRAINING;
		break;
	default:
		session = PREDICTION;
		break;
	}

	switch((opcode >> 16) & 0x3) {
	case 0:
		option = GIVEN;
		break;
	case 1:
		option = RAND;
		break;
	default:
		option = PREV;
		break;
	}

	nb_iterations = ((opcode >> 20) & 0xFFF) + 1;
	nb_examples = opcode & 0xFFFF;
}

void get_data()
{
	u32 i,j;

	features = new u8 [nb_examples*NB_FEATURES];
	if(session == TRAINING)
		labels = new u8[nb_examples];
	else if(session == PREDICTION)
		predictions = new u8[nb_examples];

	if(option == GIVEN)
		weights = new u16[NB_WEIGHTS1 + NB_WEIGHTS2];

	if(option == GIVEN) {
		for(i = 0; i < (NB_FEATURES+1); i++) {
			for(j = 0; j < NB_HIDDEN_NODES; j++) {
				weights[i*NB_HIDDEN_NODES + j] = u16(inbyte()) | (u16(inbyte()) << 8);
				outbyte(c_ACK);
			}
		}
		for(i = 0; i < (NB_HIDDEN_NODES+1); i++) {
			for(j = 0; j < NB_OUTPUT_NODES; j++) {
				weights[i*NB_OUTPUT_NODES + j + NB_WEIGHTS1] = u16(inbyte()) | (u16(inbyte()) << 8);
				outbyte(c_ACK);
			}
		}
	}

	for(i = 0; i < nb_examples; i++) {
		for(j = 0; j < NB_FEATURES; j++) {
			features[i*NB_FEATURES + j] = u8(inbyte());
		}
		if(session == TRAINING)
			labels[i] = u8(inbyte());

		outbyte(c_ACK);
	}
}

void send_predictions()
{
	u32 i;

	for(i = 0; i < nb_examples; i++) {
		if((implementation ==  SOFTWARE) or (!SINGLE_ENDED))
			outbyte(predictions[i]+1); // stored predictions are [0;nb_classes-1]
		else
			outbyte(predictions[i]);
		while(1) {
			if(inbyte() == c_ACK)
				break;
		}
	}
}
