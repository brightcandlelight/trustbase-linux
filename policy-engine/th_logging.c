#include <sys/klog.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "th_logging.h"

/* Useful trick for windows */
#ifdef WIN32
    #define my_vsnprintf _vsnprintf
#else
    #define my_vsnprintf vsnprintf
#endif

const char *log_name = NULL;
FILE *log_file = NULL;
thlog_level_t minimum_level = LOG_WARNING;

int thlog_init(const char *log_file_name, thlog_level_t min_level) {
	// TESTING
	// Write log
	log_file = fopen(log_file_name, "a");
	if (log_file == NULL) {
		return 1;
	}
	minimum_level = min_level;
	
	return 0;
}

int thlog(thlog_level_t level, const char* format, ... ) {
	char* extended_format;
	va_list args;
	time_t current_time;

	
	// If the log level is below the minimum, ditch it
	if (minimum_level > level) {
		return 0;
	}
	// Check the file
	if (log_file == NULL) {
		return 1;
	}
	// Parse the args
	va_start(args, format);
	
	// Extend the format
	// Add the time and log level
	current_time = time(NULL);
	extended_format = (char*)malloc(strlen(format) + 25 + 7 + 1 + 1);
	memcpy(extended_format, asctime(gmtime(&current_time)), 24);
	switch (level) {
	case LOG_DEBUG:
		strncat(extended_format, " :DBG: ", 7);
		break;
	case LOG_INFO:
		strncat(extended_format, " :INF: ", 7);
		break;
	case LOG_WARNING:
		strncat(extended_format, " :WRN: ", 7);
		break;
	case LOG_ERROR:
		strncat(extended_format, " :ERR: ", 7);
		break;
	}
	strncat(extended_format, format, strlen(format));
	strncat(extended_format, "\n", 1);
	
	// Write to the log
	vfprintf(log_file, extended_format, args);
	fflush(log_file);
	
	va_end(args);
	return 0;
} 

void thlog_close() {	
	if (log_file == NULL) {
		return;
	}

	fclose(log_file);
}

/*int main(int argc, char** argv) {
	thlog_level_t min;
	thlog_level_t text;
	if (argc < 5) {
		printf("Useage : %s <level> <file> <level> <text>\n", argv[0]);
		return 0;
	}
	switch (argv[1][0]) {
		case '0':
			min = LOG_DEBUG;
			break;
		case '1':
			min = LOG_INFO;
			break;
		case '2':
			min = LOG_WARNING;
			break;
		case '3':
			min = LOG_ERROR;
			break;
	}
	switch (argv[3][0]) {
		case '0':
			text = LOG_DEBUG;
			break;
		case '1':
			text = LOG_INFO;
			break;
		case '2':
			text = LOG_WARNING;
			break;
		case '3':
			text = LOG_ERROR;
			break;
	}
	thlog_init(argv[2], min); 
	thlog(text, "test:%s", argv[4]);
	return 0;
}*/