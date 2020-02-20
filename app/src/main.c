#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>

#include <gpib/ib.h>

// === config
#define HANTEK_TMC "/dev/usbtmc0"
#define PPS_GPIB_NAME "AKIP-1142/3G"
#define VM_GPIB_NAME "AKIP-V7-78/1"

// === time
#define STEP_DELAY 1.0e6 // us

// === pps
#define VOLTAGE_MIN 0.0
#define VOLTAGE_MAX 10.0
#define VOLTAGE_STEP 0.1
#define CURRENT_MAX 0.1

// === threads
void *commander(void *);
void *worker(void *);

// === utils
int get_run();
void set_run(int run_new);
double get_time();
int gpib_write(int fd, const char *str);
int gpib_read(int fd, char *buf, long len);
void gpib_print_error(int fd);


// === global variables
char dir_str[100];
pthread_rwlock_t run_lock;
int run;
char filename_vac[100];

// === program entry point
int main(int argc, char const *argv[])
{
	int ret = 0;
	int status;

	time_t start_time;
	struct tm start_time_struct;

	pthread_t t_commander;
	pthread_t t_worker;

	// === check input parameters
	if (argc < 2)
	{
		fprintf(stderr, "# E: Usage: vac <experiment_name>\n");
		ret = -1;
		goto main_exit;
	}

	// === get start time of experiment
	start_time = time(NULL);
	localtime_r(&start_time, &start_time_struct);

	// === we need actual information w/o buffering
	setlinebuf(stdout);
	setlinebuf(stderr);

	// === initialize run state variable
	pthread_rwlock_init(&run_lock, NULL);
	run = 1;

	// === create dirictory in "20191012_153504_<experiment_name>" format
	snprintf(dir_str, 100, "%04d-%02d-%02d_%02d-%02d-%02d_%s",
		start_time_struct.tm_year + 1900,
		start_time_struct.tm_mon + 1,
		start_time_struct.tm_mday,
		start_time_struct.tm_hour,
		start_time_struct.tm_min,
		start_time_struct.tm_sec,
		argv[1]
	);
	status = mkdir(dir_str, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	if (status == -1)
	{
		fprintf(stderr, "# E: unable to create experiment directory (%s)\n", strerror(errno));
		ret = -2;
		goto main_exit;
	}

	// === create file names
	snprintf(filename_vac, 100, "%s/vac.dat", dir_str);
	// printf("filename_vac \"%s\"\n", filename_vac);

	// === now start threads
	pthread_create(&t_commander, NULL, commander, NULL);
	pthread_create(&t_worker, NULL, worker, NULL);

	// === and wait ...
	pthread_join(t_worker, NULL);

	// === cancel commander thread becouse we don't need it anymore
	// === and wait for cancelation finish
	pthread_cancel(t_commander);
	pthread_join(t_commander, NULL);

	main_exit:
	return ret;
}

// === commander function
void *commander(void *arg)
{
	(void) arg;

	char str[100];
	char *s;
	int ccount;

	while(get_run())
	{
		printf("> ");

		s = fgets(str, 100, stdin);
		if (s == NULL)
		{
			fprintf(stderr, "# E: Exit\n");
			set_run(0);
			break;
		}

		switch(str[0])
		{
			case 'h':
				printf(
					"Help:\n"
					"\th -- this help;\n"
					"\tq -- exit the program;\n");
				break;
			case 'q':
				set_run(0);
				break;
			default:
				ccount = strlen(str)-1;
				fprintf(stderr, "# E: Unknown command (%.*s)\n", ccount, str);
				break;
		}
	}

	return NULL;
}

// === worker function
void *worker(void *arg)
{
	(void) arg;

	int r;

	int pps_fd;
	int vm_fd;

	int    vac_index;
	double vac_time;
	double pps_voltage;
	double pps_current;
	double vm_current;

	double voltage;

	FILE  *vac_fp;
	FILE  *gp;
	char   buf[100];

	// === first we are connecting to instruments
	r = ibfind(PPS_GPIB_NAME);
	if(r == -1)
	{
		fprintf(stderr, "# E: Unable to open power supply (%d)\n", r);
		goto worker_pps_ibfind;
	}
	pps_fd = r;

	r = ibfind(VM_GPIB_NAME);
	if(r == -1)
	{
		fprintf(stderr, "# E: Unable to open voltmeter (%d)\n", r);
		goto worker_vm_ibfind;
	}
	vm_fd = r;

#define DEBUG

	// === init pps
	gpib_write(pps_fd, "output 0");
	gpib_write(pps_fd, "instrument:nselect 1");
	gpib_write(pps_fd, "voltage:protection:clear");
	gpib_write(pps_fd, "voltage:protection:state on");
	gpib_write(pps_fd, "voltage:protection:level 5.5V");
	gpib_write(pps_fd, "voltage 5.0");
	gpib_write(pps_fd, "current 0.5");
	gpib_write(pps_fd, "channel:output 1");
	gpib_write(pps_fd, "instrument:nselect 2");
	gpib_write(pps_fd, "voltage:protection:clear");
	gpib_write(pps_fd, "voltage:protection:state on");
	gpib_write(pps_fd, "voltage:protection:level 11V");
	gpib_write(pps_fd, "voltage 0.0");
	gpib_write(pps_fd, "current 0.1");
	gpib_write(pps_fd, "channel:output 1");
	// gpib_print_error(pps_fd);


	// === init vm
	gpib_write(vm_fd, "function \"current:dc\"");
	gpib_write(vm_fd, "current:dc:range:auto on");
	gpib_write(vm_fd, "current:dc:nplcycles 10");
	gpib_write(vm_fd, "trigger:source immediate");
	gpib_write(vm_fd, "trigger:delay:auto off");
	gpib_write(vm_fd, "trigger:delay 0");
	gpib_write(vm_fd, "trigger:count 1");
	gpib_write(vm_fd, "sample:count 1");
	// gpib_print_error(vm_fd);

	// === create vac file
	vac_fp = fopen(filename_vac, "w+");
	if(vac_fp == NULL)
	{
		fprintf(stderr, "# E: Unable to open file \"%s\" (%s)\n", filename_vac, strerror(ferror(vac_fp)));
		goto worker_vac_fopen;
	}
	setlinebuf(vac_fp);

	// === write vac header
	r = fprintf(vac_fp,
		"# 1: index\n"
		"# 2: time, s\n"
		"# 3: pps voltage, V\n"
		"# 4: pps current, A\n"
		"# 5: vm curren, A\n");
	if(r < 0)
	{
		fprintf(stderr, "# E: Unable to print to file \"%s\" (%s)\n", filename_vac, strerror(r));
		goto worker_vac_header;
	}

	// === open gnuplot
	snprintf(buf, 100, "gnuplot > %s/gnuplot.log 2>&1", dir_str);
	gp = popen(buf, "w");
	if (gp == NULL)
	{
		fprintf(stderr, "# E: unable to open gnuplot pipe (%s)\n", strerror(errno));
		goto worker_gp_popen;
	}
	setlinebuf(gp);

	// === prepare gnuplot
	r = fprintf(gp,
		"set xrange [0:10]\n"
		"set xlabel \"Voltage, V\"\n"
		"set ylabel \"Current, A\"\n"
	);
	if(r < 0)
	{
		fprintf(stderr, "# E: Unable to print to gp (%s)\n", strerror(r));
		goto worker_gp_settings;
	}

	// === let the action begins!
	vac_index = 0;

	while(get_run())
	{
		voltage = vac_index * VOLTAGE_STEP;
		if (voltage > VOLTAGE_MAX)
		{
			set_run(0);
			break;
		}

		snprintf(buf, 100, "voltage %.3lf", voltage);
		gpib_write(pps_fd, buf);

		usleep(STEP_DELAY);

		vac_time = get_time();
		if (vac_time < 0)
		{
			fprintf(stderr, "# E: Unable to get time\n");
			set_run(0);
			break;
		}

		// fprintf(stderr, "vac_index = %d\n", vac_index);
		// fprintf(stderr, "vac_time = %le\n", vac_time);

		gpib_write(pps_fd, "measure:voltage?");
		gpib_read(pps_fd, buf, 100);
		// fprintf(stderr, "pps_voltage = %s\n", buf);
		pps_voltage = atof(buf);
		// fprintf(stderr, "pps_voltage = %.3le\n", pps_voltage);

		gpib_write(pps_fd, "measure:current?");
		gpib_read(pps_fd, buf, 100);
		// fprintf(stderr, "pps_current = %s\n", buf);
		pps_current = atof(buf);
		// fprintf(stderr, "pps_current = %.3le\n", pps_current);

		gpib_write(vm_fd, "read?");
		gpib_read(vm_fd, buf, 100);
		// fprintf(stderr, "vm_current = %s\n", buf);
		vm_current = atof(buf);
		// fprintf(stderr, "vm_current = %.8le\n", vm_current);

//*
		r = fprintf(vac_fp, "%d\t%le\t%.3le\t%.3le\t%.8le\n", vac_index, vac_time, pps_voltage, pps_current, vm_current);
		if(r < 0)
		{
			fprintf(stderr, "# E: Unable to print to file \"%s\" (%s)\n", filename_vac, strerror(r));
			set_run(0);
			break;
		}

		r = fprintf(gp,
			"set title \"i = %d, t = %lf s\"\n"
			"plot \"%s\" u 3:5 w l lw 1 notitle\n",
			vac_index,
			vac_time,
			filename_vac
		);
		if(r < 0)
		{
			fprintf(stderr, "# E: Unable to print to gp (%s)\n", strerror(r));
			set_run(0);
			break;
		}
//*/
		vac_index++;
	}

	gpib_write(pps_fd, "output 0");
	gpib_write(pps_fd, "voltage 0");

	worker_gp_settings:

	r = pclose(gp);
	if (r == -1)
	{
		fprintf(stderr, "# E: Unable to close gnuplot pipe (%s)\n", strerror(errno));
	}
	worker_gp_popen:


	worker_vac_header:

	r = fclose(vac_fp);
	if (r == EOF)
	{
		fprintf(stderr, "# E: Unable to close file \"%s\" (%s)\n", filename_vac, strerror(errno));
	}
	worker_vac_fopen:

	ibclr(vm_fd);
	gpib_write(vm_fd, "*rst");
	sleep(1);
	ibloc(vm_fd);
	worker_vm_ibfind:

	ibclr(pps_fd);
	gpib_write(pps_fd, "*rst");
	sleep(1);
	ibloc(pps_fd);
	worker_pps_ibfind:

	return NULL;
}

// === utils
int get_run()
{
	int run_local;
	pthread_rwlock_rdlock(&run_lock);
		run_local = run;
	pthread_rwlock_unlock(&run_lock);
	return run_local;
}

void set_run(int run_new)
{
	pthread_rwlock_wrlock(&run_lock);
		run = run_new;
	pthread_rwlock_unlock(&run_lock);
}

double get_time()
{
	static int first = 1;
	static struct timeval t_first = {0};
	struct timeval t = {0};
	double ret;
	int r;

	if (first == 1)
	{
		r = gettimeofday(&t_first, NULL);
		if (r == -1)
		{
			fprintf(stderr, "# E: unable to get time (%s)\n", strerror(errno));
			ret = -1;
		}
		else
		{
			ret = 0.0;
			first = 0;
		}
	}
	else
	{
		r = gettimeofday(&t, NULL);
		if (r == -1)
		{
			fprintf(stderr, "# E: unable to get time (%s)\n", strerror(errno));
			ret = -2;
		}
		else
		{
			ret = (t.tv_sec - t_first.tv_sec) * 1e6 + (t.tv_usec - t_first.tv_usec);
			ret /= 1e6;
		}
	}

	return ret;
}


int gpib_write(int fd, const char *str)
{
	return ibwrt(fd, str, strlen(str));
}

int gpib_read(int fd, char *buf, long len)
{
	int r;

	r = ibrd(fd, buf, len);
	if (ibcnt < len)
	{
		buf[ibcnt] = 0;
	}

	return r;
}

void gpib_print_error(int fd)
{
#ifdef DEBUG
	char buf[100] = {0};
	gpib_write(fd, "system:error?");
	gpib_read(fd, buf, 100);
	fprintf(stderr, "[debug] error = %s\n", buf);
#endif
}