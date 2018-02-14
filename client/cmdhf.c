//-----------------------------------------------------------------------------
// Copyright (C) 2010 iZsh <izsh at fail0verflow.com>
// Merlok - 2017
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// High frequency commands
//-----------------------------------------------------------------------------
#include "cmdhf.h"

static int CmdHelp(const char *Cmd);

int usage_hf_list(){
	PrintAndLog("List protocol data in trace buffer.");
	PrintAndLog("Usage:  hf list <protocol> [f][c]");
	PrintAndLog("    f      - show frame delay times as well");
	PrintAndLog("    c      - mark CRC bytes");
	PrintAndLog("Supported <protocol> values:");
	PrintAndLog("    raw    - just show raw data without annotations");
	PrintAndLog("    14a    - interpret data as iso14443a communications");
	PrintAndLog("    mf     - interpret data as iso14443a communications and decrypt crypto1 stream");
	PrintAndLog("    14b    - interpret data as iso14443b communications");
	PrintAndLog("    15     - interpret data as iso15693 communications");
	PrintAndLog("    des    - interpret data as DESFire communications");
#ifdef WITH_EMV
	PrintAndLog("    emv    - interpret data as EMV / communications");
#endif	
	PrintAndLog("    iclass - interpret data as iclass communications");
	PrintAndLog("    topaz  - interpret data as topaz communications");
	PrintAndLog("    7816   - interpret data as iso7816-4 communications");
	PrintAndLog("    legic  - interpret data as LEGIC communications");
	PrintAndLog("    felica - interpret data as ISO18092 / FeliCa communications");
	PrintAndLog("");
	PrintAndLog("Examples:");
	PrintAndLog("        hf list 14a f");
	PrintAndLog("        hf list iclass");
	return 0;
}
int usage_hf_search(){
	PrintAndLog("Usage: hf search");
	PrintAndLog("Will try to find a HF read out of the unknown tag. Stops when found.");
	PrintAndLog("Options:");
	PrintAndLog("       h	- This help");
	PrintAndLog("");
	return 0;
}
int usage_hf_snoop(){
	PrintAndLog("Usage: hf snoop <skip pairs> <skip triggers>");
	PrintAndLog("The high frequence snoop will assign all available memory on device for snooped data");
	PrintAndLog("User the 'data samples' command to download from device,  and 'data plot' to look at it");
	PrintAndLog("Press button to quit the snooping.");
	PrintAndLog("Options:");
	PrintAndLog("       h				- This help");
	PrintAndLog("       <skip pairs>	- skip sample pairs");
	PrintAndLog("       <skip triggers>	- skip number of triggers");
	PrintAndLog("");
	PrintAndLog("Examples:");
	PrintAndLog("           hf snoop");
	PrintAndLog("           hf snoop 1000 0");
	return 0;
}

bool is_last_record(uint16_t tracepos, uint8_t *trace, uint16_t traceLen) {
	return(tracepos + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) >= traceLen);
}

bool next_record_is_response(uint16_t tracepos, uint8_t *trace) {
	uint16_t next_records_datalen = *((uint16_t *)(trace + tracepos + sizeof(uint32_t) + sizeof(uint16_t)));	
	return(next_records_datalen & 0x8000);
}

bool merge_topaz_reader_frames(uint32_t timestamp, uint32_t *duration, uint16_t *tracepos, uint16_t traceLen,
								uint8_t *trace, uint8_t *frame, uint8_t *topaz_reader_command, uint16_t *data_len) {

#define MAX_TOPAZ_READER_CMD_LEN	16

	uint32_t last_timestamp = timestamp + *duration;

	if ((*data_len != 1) || (frame[0] == TOPAZ_WUPA) || (frame[0] == TOPAZ_REQA)) return false;

	memcpy(topaz_reader_command, frame, *data_len);

	while (!is_last_record(*tracepos, trace, traceLen) && !next_record_is_response(*tracepos, trace)) {
		uint32_t next_timestamp = *((uint32_t *)(trace + *tracepos));
		*tracepos += sizeof(uint32_t);
		uint16_t next_duration = *((uint16_t *)(trace + *tracepos));
		*tracepos += sizeof(uint16_t);
		uint16_t next_data_len = *((uint16_t *)(trace + *tracepos)) & 0x7FFF;
		*tracepos += sizeof(uint16_t);
		uint8_t *next_frame = (trace + *tracepos);
		*tracepos += next_data_len;
		if ((next_data_len == 1) && (*data_len + next_data_len <= MAX_TOPAZ_READER_CMD_LEN)) {
			memcpy(topaz_reader_command + *data_len, next_frame, next_data_len);
			*data_len += next_data_len;
			last_timestamp = next_timestamp + next_duration;
		} else {
			// rewind and exit
			*tracepos = *tracepos - next_data_len - sizeof(uint16_t) - sizeof(uint16_t) - sizeof(uint32_t);
			break;
		}
		uint16_t next_parity_len = (next_data_len-1)/8 + 1;
		*tracepos += next_parity_len;
	}

	*duration = last_timestamp - timestamp;
	
	return true;
}

uint16_t printTraceLine(uint16_t tracepos, uint16_t traceLen, uint8_t *trace, uint8_t protocol, bool showWaitCycles, bool markCRCBytes) {
	// sanity check
	if (tracepos + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) > traceLen) return traceLen;

	bool isResponse;
	uint16_t data_len, parity_len;
	uint32_t duration, timestamp, first_timestamp, EndOfTransmissionTimestamp;
	uint8_t topaz_reader_command[9];
	char explanation[30] = {0};
	uint8_t mfData[32] = {0};
	size_t mfDataLen = 0;

		
	first_timestamp = *((uint32_t *)(trace));
	timestamp = *((uint32_t *)(trace + tracepos));
	tracepos += 4;

	duration = *((uint16_t *)(trace + tracepos));
	tracepos += 2;

	data_len = *((uint16_t *)(trace + tracepos));
	tracepos += 2;

	if (data_len & 0x8000) {
		data_len &= 0x7fff;
		isResponse = true;
	} else {
		isResponse = false;
	}
	parity_len = (data_len-1)/8 + 1;

	if (tracepos + data_len + parity_len > traceLen) {
		return traceLen;
	}
	uint8_t *frame = trace + tracepos;
	tracepos += data_len;
	uint8_t *parityBytes = trace + tracepos;
	tracepos += parity_len;

	if (protocol == TOPAZ && !isResponse) {
		// topaz reader commands come in 1 or 9 separate frames with 7 or 8 Bits each.
		// merge them:
		if (merge_topaz_reader_frames(timestamp, &duration, &tracepos, traceLen, trace, frame, topaz_reader_command, &data_len)) {
			frame = topaz_reader_command;
		}
	}
	
	//Check the CRC status
	uint8_t crcStatus = 2;

	if (data_len > 2) {
		switch (protocol) {
			case ICLASS:
				crcStatus = iclass_CRC_check(isResponse, frame, data_len);
				break;
			case ISO_14443B:
			case TOPAZ:
			case FELICA:
				crcStatus = iso14443B_CRC_check(frame, data_len);
				break;
			case PROTO_MIFARE:
				crcStatus = mifare_CRC_check(isResponse, frame, data_len);		
			case ISO_14443A:
			case MFDES:
				crcStatus = iso14443A_CRC_check(isResponse, frame, data_len);
				break;
			case ISO_15693:
				crcStatus = iso15693_CRC_check(frame, data_len);
				break;
			default: 
				break;
		}
	}
	//0 CRC-command, CRC not ok
	//1 CRC-command, CRC ok
	//2 Not crc-command

	//--- Draw the data column
	char line[16][110];

	for (int j = 0; j < data_len && j/16 < 16; j++) {

		uint8_t parityBits = parityBytes[j>>3];
		if (protocol != LEGIC &&
			protocol != ISO_14443B && 
			protocol != ISO_7816_4 &&
			(isResponse || protocol == ISO_14443A) &&
			(oddparity8(frame[j]) != ((parityBits >> (7-(j&0x0007))) & 0x01))) {
			snprintf(line[j/16]+(( j % 16) * 4),110, "%02x! ", frame[j]);
		} else {
			snprintf(line[j/16]+(( j % 16) * 4),110, "%02x  ", frame[j]);
		}

	}

	if (markCRCBytes) {
		//CRC-command
		if(crcStatus == 0 || crcStatus == 1) {
			char *pos1 = line[(data_len-2)/16]+(((data_len-2) % 16) * 4);
			(*pos1) = '[';
			char *pos2 = line[(data_len)/16]+(((data_len) % 16) * 4);
			sprintf(pos2, "%c", ']');
		}
	}

	if (data_len == 0 ) {
		sprintf(line[0],"<empty trace - possible error>");
		return tracepos;
	}

	// Draw the CRC column
	char *crc = (crcStatus == 0 ? "!crc" : (crcStatus == 1 ? " ok " : "    "));

	EndOfTransmissionTimestamp = timestamp + duration;

	// Always annotate LEGIC read/tag
	if ( protocol == LEGIC )
		annotateLegic(explanation, sizeof(explanation), frame, data_len);

	if ( protocol == PROTO_MIFARE )
		annotateMifare(explanation, sizeof(explanation), frame, data_len, parityBytes, parity_len, isResponse);
		
	if (!isResponse)	{
		switch(protocol) {
			case ICLASS:		annotateIclass(explanation,sizeof(explanation),frame,data_len); break;
			case ISO_14443A:	annotateIso14443a(explanation,sizeof(explanation),frame,data_len); break;
			case MFDES:			annotateMfDesfire(explanation,sizeof(explanation),frame,data_len); break;
			case ISO_14443B:	annotateIso14443b(explanation,sizeof(explanation),frame,data_len); break;
			case TOPAZ:			annotateTopaz(explanation,sizeof(explanation),frame,data_len); break;
			case ISO_7816_4:	annotateIso7816(explanation,sizeof(explanation),frame,data_len); break;
			case ISO_15693:		annotateIso15693(explanation,sizeof(explanation),frame,data_len); break;
			case FELICA:		annotateFelica(explanation,sizeof(explanation),frame,data_len); break;
			default:			break;
		}
	}

	int num_lines = MIN((data_len - 1)/16 + 1, 16);
	for (int j = 0; j < num_lines ; j++) {
		if (j == 0) {
			PrintAndLog(" %10u | %10u | %s |%-64s | %s| %s",
				(timestamp - first_timestamp),
				(EndOfTransmissionTimestamp - first_timestamp),
				(isResponse ? "Tag" : "Rdr"),
				line[j],
				(j == num_lines-1) ? crc : "    ",
				(j == num_lines-1) ? explanation : "");
		} else {
			PrintAndLog("            |            |     |%-64s | %s| %s",
				line[j],
				(j == num_lines-1) ? crc : "    ",
				(j == num_lines-1) ? explanation : "");
		}
	}

	if (DecodeMifareData(frame, data_len, parityBytes, isResponse, mfData, &mfDataLen)) {
		memset(explanation, 0x00, sizeof(explanation));
		if (!isResponse) {
			explanation[0] = '>';
			annotateIso14443a(&explanation[1], sizeof(explanation) - 1, mfData, mfDataLen);
		}
		uint8_t crcc = iso14443A_CRC_check(isResponse, mfData, mfDataLen);
		PrintAndLog("            |          * | dec |%-64s | %-4s| %s",
			sprint_hex(mfData, mfDataLen),
			(crcc == 0 ? "!crc" : (crcc == 1 ? " ok " : "    ")),
			(true) ? explanation : "");
	};

	if (is_last_record(tracepos, trace, traceLen)) return traceLen;
	
	if (showWaitCycles && !isResponse && next_record_is_response(tracepos, trace)) {
		uint32_t next_timestamp = *((uint32_t *)(trace + tracepos));
			PrintAndLog(" %10u | %10u | %s |fdt (Frame Delay Time): %d",
				(EndOfTransmissionTimestamp - first_timestamp),
				(next_timestamp - first_timestamp),
				"   ",
				(next_timestamp - EndOfTransmissionTimestamp));
		}

	return tracepos;
}

void printFelica(uint16_t traceLen, uint8_t *trace) {

	PrintAndLog("    Gap | Src | Data                            | CRC      | Annotation        |");
	PrintAndLog("--------|-----|---------------------------------|----------|-------------------|");
    uint16_t tracepos = 0;

    while( tracepos < traceLen) {

		if (tracepos + 3 >= traceLen) break;

		uint16_t gap = (uint16_t)trace[tracepos+1] + ((uint16_t)trace[tracepos] >> 8);
		uint16_t crc_ok = trace[tracepos+2];
		tracepos += 3;

		if (tracepos + 3 >= traceLen) break;

		uint16_t len = trace[tracepos+2];

		//I am stripping SYNC
		tracepos += 3; //skip SYNC

		if( tracepos + len + 1 >= traceLen) break;

		uint8_t cmd = trace[tracepos];
		uint8_t isResponse = cmd&1;

		char line[32][110];
		for (int j = 0; j < len+1 && j/8 < 32; j++) {
			snprintf(line[j/8]+(( j % 8) * 4), 110, " %02x ", trace[tracepos+j]);
		}
		char expbuf[50];
		switch(cmd) {
			case FELICA_POLL_REQ: snprintf(expbuf,49,"Poll Req");break;
			case FELICA_POLL_ACK: snprintf(expbuf,49,"Poll Resp");break;

			case FELICA_REQSRV_REQ: snprintf(expbuf,49,"Request Srvc Req");break;
			case FELICA_REQSRV_ACK: snprintf(expbuf,49,"Request Srv Resp");break;

			case FELICA_RDBLK_REQ: snprintf(expbuf,49,"Read block(s) Req");break;
			case FELICA_RDBLK_ACK: snprintf(expbuf,49,"Read block(s) Resp");break;

			case FELICA_WRTBLK_REQ: snprintf(expbuf,49,"Write block(s) Req");break;
			case FELICA_WRTBLK_ACK: snprintf(expbuf,49,"Write block(s) Resp");break;
			case FELICA_SRCHSYSCODE_REQ: snprintf(expbuf,49,"Search syscode Req");break;
			case FELICA_SRCHSYSCODE_ACK: snprintf(expbuf,49,"Search syscode Resp");break;

			case FELICA_REQSYSCODE_REQ: snprintf(expbuf,49,"Request syscode Req");break;
			case FELICA_REQSYSCODE_ACK: snprintf(expbuf,49,"Request syscode Resp");break;

			case FELICA_AUTH1_REQ: snprintf(expbuf,49,"Auth1 Req");break;
			case FELICA_AUTH1_ACK: snprintf(expbuf,49,"Auth1 Resp");break;

			case FELICA_AUTH2_REQ: snprintf(expbuf,49,"Auth2 Req");break;
			case FELICA_AUTH2_ACK: snprintf(expbuf,49,"Auth2 Resp");break;

			case FELICA_RDSEC_REQ: snprintf(expbuf,49,"Secure read Req");break;
			case FELICA_RDSEC_ACK: snprintf(expbuf,49,"Secure read Resp");break;

			case FELICA_WRTSEC_REQ: snprintf(expbuf,49,"Secure write Req");break;
			case FELICA_WRTSEC_ACK: snprintf(expbuf,49,"Secure write Resp");break;

			case FELICA_REQSRV2_REQ: snprintf(expbuf,49,"Request Srvc v2 Req");break;
			case FELICA_REQSRV2_ACK: snprintf(expbuf,49,"Request Srvc v2 Resp");break;

			case FELICA_GETSTATUS_REQ: snprintf(expbuf,49,"Get status Req");break;
			case FELICA_GETSTATUS_ACK: snprintf(expbuf,49,"Get status Resp");break;

			case FELICA_OSVER_REQ: snprintf(expbuf,49,"Get OS Version Req");break;
			case FELICA_OSVER_ACK: snprintf(expbuf,49,"Get OS Version Resp");break;

			case FELICA_RESET_MODE_REQ: snprintf(expbuf,49,"Reset mode Req");break;
			case FELICA_RESET_MODE_ACK: snprintf(expbuf,49,"Reset mode Resp");break;

			case FELICA_AUTH1V2_REQ: snprintf(expbuf,49,"Auth1 v2 Req");break;
			case FELICA_AUTH1V2_ACK: snprintf(expbuf,49,"Auth1 v2 Resp");break;

			case FELICA_AUTH2V2_REQ: snprintf(expbuf,49,"Auth2 v2 Req");break;
			case FELICA_AUTH2V2_ACK: snprintf(expbuf,49,"Auth2 v2 Resp");break;

			case FELICA_RDSECV2_REQ: snprintf(expbuf,49,"Secure read v2 Req");break;
			case FELICA_RDSECV2_ACK: snprintf(expbuf,49,"Secure read v2 Resp");break;
			case FELICA_WRTSECV2_REQ: snprintf(expbuf,49,"Secure write v2 Req");break;
			case FELICA_WRTSECV2_ACK: snprintf(expbuf,49,"Secure write v2 Resp");break;

			case FELICA_UPDATE_RNDID_REQ: snprintf(expbuf,49,"Update IDr Req");break;
			case FELICA_UPDATE_RNDID_ACK: snprintf(expbuf,49,"Update IDr Resp");break;
			default: snprintf(expbuf,49,"Unknown");break;
		}
		
		int num_lines = MIN((len )/16 + 1, 16);
		for (int j = 0; j < num_lines ; j++) {
			if (j == 0) {
				PrintAndLog("%7d | %s |%-32s |%02x %02x %s| %s",
					gap,
					(isResponse ? "Tag" : "Rdr"),
					line[j],
					trace[tracepos+len],
					trace[tracepos+len+1],
					(crc_ok) ? "OK" : "NG",
					expbuf);
			} else {
				PrintAndLog("        |     |%-32s |        |    ", line[j]);
			}
		}
		tracepos += len + 1;
	}
    PrintAndLog("");
}

int CmdHFList(const char *Cmd) {
	clearCommandBuffer();
		
	bool showWaitCycles = false;
	bool markCRCBytes = false;
	char type[10] = {0};
	//int tlen = param_getstr(Cmd,0,type);
	char param1 = param_getchar(Cmd, 1);
	char param2 = param_getchar(Cmd, 2);
	bool errors = false;
	uint8_t protocol = 0;

	//Validate params H or empty
	if (strlen(Cmd) < 1 || param1 == 'h' || param1 == 'H') return usage_hf_list();
	
	//Validate params  F,C
	if(
		(param1 != 0 && param1 != 'f' && param1 != 'c')	|| 
		(param2 != 0 && param2 != 'f' && param2 != 'c')
		) {
		return usage_hf_list();
	}

	param_getstr(Cmd, 0, type, sizeof(type) );
	
	// validate type of output
	if (strcmp(type,     "iclass") == 0)	protocol = ICLASS;
	else if(strcmp(type, "14a") == 0)		protocol = ISO_14443A;
	else if(strcmp(type, "14b") == 0)		protocol = ISO_14443B;
	else if(strcmp(type, "topaz") == 0)		protocol = TOPAZ;
	else if(strcmp(type, "7816") == 0)		protocol = ISO_7816_4;	
	else if(strcmp(type, "des") == 0)		protocol = MFDES;
	else if(strcmp(type, "legic") == 0)		protocol = LEGIC;
	else if(strcmp(type, "15") == 0)		protocol = ISO_15693;
	else if(strcmp(type, "felica") == 0)	protocol = FELICA;
	else if(strcmp(type, "mf") == 0)		protocol = PROTO_MIFARE;
	else if(strcmp(type, "raw") == 0)		protocol = -1;//No crc, no annotations
	else errors = true;

	if (errors) return usage_hf_list();

	if (param1 == 'f' || param2 == 'f') showWaitCycles = true;
	if (param1 == 'c' || param2 == 'c') markCRCBytes = true;

	uint8_t *trace;
	uint16_t tracepos = 0;
	trace = malloc(USB_CMD_DATA_SIZE);

	// Query for the size of the trace
	UsbCommand response;
	GetFromBigBuf(trace, USB_CMD_DATA_SIZE, 0);
	if ( !WaitForResponseTimeout(CMD_ACK, &response, 4000) ) {
		PrintAndLog("timeout while waiting for reply.");
		return 1;
	}
	
	uint16_t traceLen = response.arg[2];
	if (traceLen > USB_CMD_DATA_SIZE) {
		uint8_t *p = realloc(trace, traceLen);
		if (p == NULL) {
			PrintAndLog("Cannot allocate memory for trace");
			free(trace);
			return 2;
		}
		trace = p;
		GetFromBigBuf(trace, traceLen, 0);
		WaitForResponse(CMD_ACK, NULL);
	}
	
	PrintAndLog("Recorded Activity (TraceLen = %d bytes)", traceLen);
	PrintAndLog("");
	if (protocol == FELICA) {
		printFelica(traceLen, trace);
	} else { 
		PrintAndLog("Start = Start of Start Bit, End = End of last modulation. Src = Source of Transfer");
		if ( protocol == ISO_14443A || protocol == PROTO_MIFARE)
			PrintAndLog("iso14443a - All times are in carrier periods (1/13.56Mhz)");
		if ( protocol == ICLASS )
			PrintAndLog("iClass    - Timings are not as accurate");
		if ( protocol == LEGIC )
			PrintAndLog("LEGIC    - Timings are in ticks (1us == 1.5ticks)");
		if ( protocol == ISO_15693 )
			PrintAndLog("ISO15693 - Timings are not as accurate");
		if ( protocol == FELICA )
			PrintAndLog("ISO18092 / FeliCa - Timings are not as accurate");	
		
		PrintAndLog("");
		PrintAndLog("      Start |        End | Src | Data (! denotes parity error)                                   | CRC | Annotation         |");
		PrintAndLog("------------|------------|-----|-----------------------------------------------------------------|-----|--------------------|");

		ClearAuthData();
		while(tracepos < traceLen) {
			tracepos = printTraceLine(tracepos, traceLen, trace, protocol, showWaitCycles, markCRCBytes);
		}
	}
	free(trace);
	return 0;
}

int CmdHFSearch(const char *Cmd){

	char cmdp = param_getchar(Cmd, 0);	
	if (cmdp == 'h' || cmdp == 'H') return usage_hf_search();
	
	PrintAndLog("");
	int ans = CmdHF14AInfo("s");
	if (ans > 0) {
		PrintAndLog("\nValid ISO14443-A Tag Found\n");
		return ans;
	} 
	ans = HF15Reader("", false);
	if (ans) {
		PrintAndLog("\nValid ISO15693 Tag Found\n");
		return ans;
	}
	ans = HFLegicReader("", false);
	if ( ans == 0) {
		PrintAndLog("\nValid LEGIC Tag Found\n");
		return 1;
	}
	ans = CmdHFTopazReader("s");
	if (ans == 0) {
		PrintAndLog("\nValid Topaz Tag Found\n");
		return 1;
	}
	// 14b and iclass is the longest test (put last)
	ans = HF14BReader(false); //CmdHF14BReader("s");
	if (ans) {
		PrintAndLog("\nValid ISO14443-B Tag Found\n");
		return ans;
	}
	ans = HFiClassReader("", false, false);
	if (ans) {
		PrintAndLog("\nValid iClass Tag (or PicoPass Tag) Found\n");
		return ans;
	}

	/*
	ans = CmdHFFelicaReader("s");
	if (ans) {
		PrintAndLog("\nValid ISO18092 / FeliCa Found\n");
		return ans;
	}
	*/
	
	PrintAndLog("\nno known/supported 13.56 MHz tags found\n");
	return 0;
}

int CmdHFTune(const char *Cmd) {
	PrintAndLog("[+] Measuring HF antenna, press button to exit");
	UsbCommand c = {CMD_MEASURE_ANTENNA_TUNING_HF};
	clearCommandBuffer();
	SendCommand(&c);
	return 0;
}

int CmdHFSnoop(const char *Cmd) {
	char cmdp = param_getchar(Cmd, 0);	
	if (cmdp == 'h' || cmdp == 'H') return usage_hf_snoop();
	
	int skippairs =  param_get32ex(Cmd, 0, 0, 10);
	int skiptriggers =  param_get32ex(Cmd, 1, 0, 10);
	
	UsbCommand c = {CMD_HF_SNIFFER, {skippairs, skiptriggers, 0}};
	clearCommandBuffer();
	SendCommand(&c);
	return 0;
}

static command_t CommandTable[] = {
	{"help",        CmdHelp,          1, "This help"},
	{"14a",         CmdHF14A,         1, "{ ISO14443A RFIDs...            }"},
	{"14b",         CmdHF14B,         1, "{ ISO14443B RFIDs...            }"},
	{"15",          CmdHF15,          1, "{ ISO15693 RFIDs...             }"},
	{"epa",         CmdHFEPA,         1, "{ German Identification Card... }"},
	{"emv",         CmdHFEMV,         1, "{ EMV RFIDs...                  }"},
	{"felica",      CmdHFFelica,      1, "{ ISO18092 / Felica RFIDs...    }"},
	{"legic",       CmdHFLegic,       1, "{ LEGIC RFIDs...                }"},
	{"iclass",      CmdHFiClass,      1, "{ ICLASS RFIDs...               }"},
	{"mf",      	CmdHFMF,		  1, "{ MIFARE RFIDs...               }"},
	{"mfu",         CmdHFMFUltra,     1, "{ MIFARE Ultralight RFIDs...    }"},
	{"mfdes",		CmdHFMFDes,		  1, "{ MIFARE Desfire RFIDs...       }"},
	{"topaz",		CmdHFTopaz,		  1, "{ TOPAZ (NFC Type 1) RFIDs...   }"},
	{"tune",		CmdHFTune,	      0, "Continuously measure HF antenna tuning"},
	{"list",        CmdHFList,        1, "List protocol data in trace buffer"},
	{"search",      CmdHFSearch,      1, "Search for known HF tags [preliminary]"},
	{"snoop",       CmdHFSnoop,       0, "<samples to skip (10000)> <triggers to skip (1)> Generic HF Snoop"},
	{NULL, NULL, 0, NULL}
};

int CmdHF(const char *Cmd) {
	clearCommandBuffer();
	CmdsParse(CommandTable, Cmd);
	return 0; 
}

int CmdHelp(const char *Cmd) {
	CmdsHelp(CommandTable);
	return 0;
}
