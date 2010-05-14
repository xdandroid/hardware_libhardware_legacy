//code comes from qemu gps code
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

#define  LOG_TAG  "gps_msm7k"
#include <cutils/log.h>
#include <cutils/sockets.h>
#include <hardware_legacy/gps.h>

#define  GPS_DEBUG  1

#if GPS_DEBUG
#  define  D(...)   LOGD(__VA_ARGS__)
#else
#  define  D(...)   ((void)0)
#endif


/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       N M E A   T O K E N I Z E R                     *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

/* this is the state of our connection */

typedef struct {
    const char*  p;
    const char*  end;
} Token;

#define  MAX_NMEA_TOKENS  32

typedef struct {
    int     count;
    Token   tokens[ MAX_NMEA_TOKENS ];
} NmeaTokenizer;

static int
nmea_tokenizer_init( NmeaTokenizer*  t, const char*  p, const char*  end )
{
    int    count = 0;
    char*  q;

    // the initial '$' is optional
    if (p < end && p[0] == '$')
        p += 1;

    // remove trailing newline
    if (end > p && end[-1] == '\n') {
        end -= 1;
        if (end > p && end[-1] == '\r')
            end -= 1;
    }

    // get rid of checksum at the end of the sentecne
    if (end >= p+3 && end[-3] == '*') {
        end -= 3;
    }

    while (p < end) {
        const char*  q = p;

        q = memchr(p, ',', end-p);
        if (q == NULL)
            q = end;

         if (count < MAX_NMEA_TOKENS) {
             t->tokens[count].p   = p;
             t->tokens[count].end = q;
             count += 1;
         }
        if (q < end)
            q += 1;

        p = q;
    }

    t->count = count;
    return count;
}

static Token
nmea_tokenizer_get( NmeaTokenizer*  t, int  index )
{
    Token  tok;
    static const char*  dummy = "";

    if (index < 0 || index >= t->count) {
        tok.p = tok.end = dummy;
    } else
        tok = t->tokens[index];

    return tok;
}


static int
str2int( const char*  p, const char*  end )
{
    int   result = 0;
    int   len    = end - p;

    for ( ; len > 0; len--, p++ )
    {
        int  c;

        if (p >= end)
            goto Fail;

        c = *p - '0';
        if ((unsigned)c >= 10)
            goto Fail;

        result = result*10 + c;
    }
    return  result;

Fail:
    return -1;
}

static double
str2float( const char*  p, const char*  end )
{
    int   result = 0;
    int   len    = end - p;
    char  temp[16];

    if (len >= (int)sizeof(temp))
        return 0.;

    memcpy( temp, p, len );
    temp[len] = 0;
    return strtod( temp, NULL );
}

/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       N M E A   P A R S E R                           *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

#define  NMEA_MAX_SIZE  255

typedef struct {
    int     pos;
    int     overflow;
    int     utc_year;
    int     utc_mon;
    int     utc_day;
    int     utc_diff;
    GpsLocation fix;
    GpsSvStatus sv_status;
    char    in[ NMEA_MAX_SIZE+1 ];
} NmeaReader;

typedef struct {
	int                     init;
	int                     fd;
	GpsCallbacks            callbacks;
	GpsStatus status;
	pthread_t               thread;
	int                     control[2];
} GpsState;
static GpsState  _gps_state[1];

static void
nmea_reader_update_utc_diff( NmeaReader*  r )
{
    time_t         now = time(NULL);
    struct tm      tm_local;
    struct tm      tm_utc;
    long           time_local, time_utc;

    gmtime_r( &now, &tm_utc );
    localtime_r( &now, &tm_local );

    time_local = tm_local.tm_sec +
                 60*(tm_local.tm_min +
                 60*(tm_local.tm_hour +
                 24*(tm_local.tm_yday +
                 365*tm_local.tm_year)));

    time_utc = tm_utc.tm_sec +
               60*(tm_utc.tm_min +
               60*(tm_utc.tm_hour +
               24*(tm_utc.tm_yday +
               365*tm_utc.tm_year)));

    r->utc_diff = time_utc - time_local;
}


static void
nmea_reader_init( NmeaReader*  r )
{
    memset( r, 0, sizeof(*r) );

    r->pos      = 0;
    r->overflow = 0;
    r->utc_year = -1;
    r->utc_mon  = -1;
    r->utc_day  = -1;

    nmea_reader_update_utc_diff( r );
}


static int
nmea_reader_update_time( NmeaReader*  r, Token  tok )
{
    int        hour, minute;
    double     seconds;
    struct tm  tm;
    time_t     fix_time;

    if (tok.p + 6 > tok.end)
        return -1;

    if (r->utc_year < 0) {
        // no date yet, get current one
        time_t  now = time(NULL);
        gmtime_r( &now, &tm );
        r->utc_year = tm.tm_year + 1900;
        r->utc_mon  = tm.tm_mon + 1;
        r->utc_day  = tm.tm_mday;
    }

    hour    = str2int(tok.p,   tok.p+2);
    minute  = str2int(tok.p+2, tok.p+4);
    seconds = str2float(tok.p+4, tok.end);

    tm.tm_hour  = hour;
    tm.tm_min   = minute;
    tm.tm_sec   = (int) seconds;
    tm.tm_year  = r->utc_year - 1900;
    tm.tm_mon   = r->utc_mon - 1;
    tm.tm_mday  = r->utc_day;
    tm.tm_isdst = -1;

    fix_time = mktime( &tm ) + r->utc_diff;
    r->fix.timestamp = (long long)fix_time * 1000+(int)(seconds*1000)%1000;

    return 0;
}

static int
nmea_reader_update_date( NmeaReader*  r, Token  date, Token  time )
{
    Token  tok = date;
    int    day, mon, year;

    if (tok.p + 6 != tok.end) {
        D("date not properly formatted: '%.*s'", tok.end-tok.p, tok.p);
        return -1;
    }
    day  = str2int(tok.p, tok.p+2);
    mon  = str2int(tok.p+2, tok.p+4);
    year = str2int(tok.p+4, tok.p+6) + 2000;

    if ((day|mon|year) < 0) {
        D("date not properly formatted: '%.*s'", tok.end-tok.p, tok.p);
        return -1;
    }

    r->utc_year  = year;
    r->utc_mon   = mon;
    r->utc_day   = day;

    return nmea_reader_update_time( r, time );
}


static double
convert_from_hhmm( Token  tok )
{
    double  val     = str2float(tok.p, tok.end);
    int     degrees = (int)(floor(val) / 100);
    double  minutes = val - degrees*100.;
    double  dcoord  = degrees + minutes / 60.0;
    return dcoord;
}


static int
nmea_reader_update_latlong( NmeaReader*  r,
                            Token        latitude,
                            char         latitudeHemi,
                            Token        longitude,
                            char         longitudeHemi )
{
    double   lat, lon;
    Token    tok;

    tok = latitude;
    if (tok.p + 6 > tok.end) {
        D("latitude is too short: '%.*s'", tok.end-tok.p, tok.p);
        return -1;
    }
    lat = convert_from_hhmm(tok);
    if (latitudeHemi == 'S')
        lat = -lat;

    tok = longitude;
    if (tok.p + 6 > tok.end) {
        D("longitude is too short: '%.*s'", tok.end-tok.p, tok.p);
        return -1;
    }
    lon = convert_from_hhmm(tok);
    if (longitudeHemi == 'W')
        lon = -lon;

    r->fix.flags    |= GPS_LOCATION_HAS_LAT_LONG;
    r->fix.latitude  = lat;
    r->fix.longitude = lon;
    return 0;
}


static int
nmea_reader_update_altitude( NmeaReader*  r,
                             Token        altitude,
                             Token        units )
{
    double  alt;
    Token   tok = altitude;

    if (tok.p >= tok.end)
        return -1;

    r->fix.flags   |= GPS_LOCATION_HAS_ALTITUDE;
    r->fix.altitude = str2float(tok.p, tok.end);
    return 0;
}


static int
nmea_reader_update_bearing( NmeaReader*  r,
                            Token        bearing )
{
    double  alt;
    Token   tok = bearing;

    if (tok.p >= tok.end)
        return -1;

    r->fix.flags   |= GPS_LOCATION_HAS_BEARING;
    r->fix.bearing  = str2float(tok.p, tok.end);
    return 0;
}


static int
nmea_reader_update_speed( NmeaReader*  r,
                          Token        speed )
{
    double  alt;
    Token   tok = speed;

    if (tok.p >= tok.end)
        return -1;

    r->fix.flags   |= GPS_LOCATION_HAS_SPEED;
    r->fix.speed    = str2float(tok.p, tok.end);
    return 0;
}


static void
nmea_reader_parse( NmeaReader*  r )
{
   /* we received a complete sentence, now parse it to generate
    * a new GPS fix...
    */
    NmeaTokenizer  tzer[1];
    Token          tok;
    int i;

    D("Received: '%.*s'", r->pos, r->in);
    if (r->pos < 9) {
        D("Too short. discarded.");
        return;
    }
    {
		struct timeval tv;
		gettimeofday(&tv, NULL);
		_gps_state->callbacks.nmea_cb(tv.tv_sec*1000+tv.tv_usec/1000, r->in, r->pos);
    }

    nmea_tokenizer_init(tzer, r->in, r->in + r->pos);
#if GPS_DEBUG
    {
        int  n;
        D("Found %d tokens", tzer->count);
        for (n = 0; n < tzer->count; n++) {
            Token  tok = nmea_tokenizer_get(tzer,n);
            D("size of %2d: '%d', ptr=%x", n, tok.end-tok.p, tok.p);
            D("%2d: '%.*s'", n, tok.end-tok.p, tok.p);
        }
    }
#endif

    tok = nmea_tokenizer_get(tzer, 0);
    if (tok.p + 5 > tok.end) {
        D("sentence id '%.*s' too short, ignored.", tok.end-tok.p, tok.p);
        return;
    }

    // ignore first two characters.
    tok.p += 2;
    if ( !memcmp(tok.p, "GGA", 3) ) {
        // GPS fix
        Token  tok_time          = nmea_tokenizer_get(tzer,1);
        Token  tok_latitude      = nmea_tokenizer_get(tzer,2);
        Token  tok_latitudeHemi  = nmea_tokenizer_get(tzer,3);
        Token  tok_longitude     = nmea_tokenizer_get(tzer,4);
        Token  tok_longitudeHemi = nmea_tokenizer_get(tzer,5);
        Token  tok_altitude      = nmea_tokenizer_get(tzer,9);
        Token  tok_altitudeUnits = nmea_tokenizer_get(tzer,10);

        nmea_reader_update_time(r, tok_time);
        nmea_reader_update_latlong(r, tok_latitude,
                                      tok_latitudeHemi.p[0],
                                      tok_longitude,
                                      tok_longitudeHemi.p[0]);
        nmea_reader_update_altitude(r, tok_altitude, tok_altitudeUnits);

    } else if ( !memcmp(tok.p, "GSA", 3) ) {
	    //Satellites are handled by RPC-side code.
    } else if ( !memcmp(tok.p, "GSV", 3) ) {
	    //Satellites are handled by RPC-side code.
    } else if ( !memcmp(tok.p, "RMC", 3) ) {
        Token  tok_time          = nmea_tokenizer_get(tzer,1);
        Token  tok_fixStatus     = nmea_tokenizer_get(tzer,2);
        Token  tok_latitude      = nmea_tokenizer_get(tzer,3);
        Token  tok_latitudeHemi  = nmea_tokenizer_get(tzer,4);
        Token  tok_longitude     = nmea_tokenizer_get(tzer,5);
        Token  tok_longitudeHemi = nmea_tokenizer_get(tzer,6);
        Token  tok_speed         = nmea_tokenizer_get(tzer,7);
        Token  tok_bearing       = nmea_tokenizer_get(tzer,8);
        Token  tok_date          = nmea_tokenizer_get(tzer,9);

        D("in RMC, fixStatus=%c", tok_fixStatus.p[0]);
        if (tok_fixStatus.p[0] == 'A')
        {
            nmea_reader_update_date( r, tok_date, tok_time );

            nmea_reader_update_latlong( r, tok_latitude,
                                           tok_latitudeHemi.p[0],
                                           tok_longitude,
                                           tok_longitudeHemi.p[0] );

            nmea_reader_update_bearing( r, tok_bearing );
            nmea_reader_update_speed  ( r, tok_speed );
        }
    } else {
        tok.p -= 2;
        D("unknown sentence '%.*s", tok.end-tok.p, tok.p);
    }
    if (r->fix.flags != 0) {
#if GPS_DEBUG
        char   temp[256];
        char*  p   = temp;
        char*  end = p + sizeof(temp);
        struct tm   utc;

        p += snprintf( p, end-p, "sending fix" );
        if (r->fix.flags & GPS_LOCATION_HAS_LAT_LONG) {
            p += snprintf(p, end-p, " lat=%g lon=%g", r->fix.latitude, r->fix.longitude);
        }
        if (r->fix.flags & GPS_LOCATION_HAS_ALTITUDE) {
            p += snprintf(p, end-p, " altitude=%g", r->fix.altitude);
        }
        if (r->fix.flags & GPS_LOCATION_HAS_SPEED) {
            p += snprintf(p, end-p, " speed=%g", r->fix.speed);
        }
        if (r->fix.flags & GPS_LOCATION_HAS_BEARING) {
            p += snprintf(p, end-p, " bearing=%g", r->fix.bearing);
        }
        if (r->fix.flags & GPS_LOCATION_HAS_ACCURACY) {
            p += snprintf(p,end-p, " accuracy=%g", r->fix.accuracy);
        }
        gmtime_r( (time_t*) &r->fix.timestamp, &utc );
        p += snprintf(p, end-p, " time=%s", asctime( &utc ) );
        D(temp);
#endif
        if (_gps_state->callbacks.location_cb) {
            _gps_state->callbacks.location_cb( &r->fix );
            r->fix.flags = 0;
        }
        else {
            D("no callback, keeping data until needed !");
        }
    }
}


static void
nmea_reader_addc( NmeaReader*  r, int  c )
{
    if (r->overflow) {
        r->overflow = (c != '\n');
        return;
    }

    if (r->pos >= (int) sizeof(r->in)-1 ) {
        r->overflow = 1;
        r->pos      = 0;
        return;
    }

    r->in[r->pos] = (char)c;
    r->pos       += 1;

    if (c == '\n') {
        nmea_reader_parse( r );
        r->pos = 0;
    }
}


/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       C O N N E C T I O N   S T A T E                 *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

/* commands sent to the gps thread */
enum {
	CMD_QUIT  = 0,
	CMD_START = 1,
	CMD_STOP  = 2
};



static void gps_state_done( GpsState*  s ) {
	// tell the thread to quit, and wait for it
	char   cmd = CMD_QUIT;
	void*  dummy;
	write( s->control[0], &cmd, 1 );
	pthread_join(s->thread, &dummy);

	// close the control socket pair
	close( s->control[0] ); s->control[0] = -1;
	close( s->control[1] ); s->control[1] = -1;

	// close connection to the QEMU GPS daemon
	close( s->fd ); s->fd = -1;
	s->init = 0;
}


static void
gps_state_start( GpsState*  s )
{
	char  cmd = CMD_START;
	int   ret;

	do { ret=write( s->control[0], &cmd, 1 ); }
	while (ret < 0 && errno == EINTR);

	if (ret != 1)
		D("%s: could not send CMD_START command: ret=%d: %s",
		__FUNCTION__, ret, strerror(errno));
}


static void gps_state_stop( GpsState*  s ) {
	char  cmd = CMD_STOP;
	int   ret;

	do { ret=write( s->control[0], &cmd, 1 ); }
	while (ret < 0 && errno == EINTR);

	if (ret != 1)
		D("%s: could not send CMD_STOP command: ret=%d: %s",
		__FUNCTION__, ret, strerror(errno));
}


static int epoll_register( int  epoll_fd, int  fd ) {
	struct epoll_event  ev;
	int                 ret, flags;

	/* important: make the fd non-blocking */
	flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	ev.events  = EPOLLIN;
	ev.data.fd = fd;
	do {
		ret = epoll_ctl( epoll_fd, EPOLL_CTL_ADD, fd, &ev );
	} while (ret < 0 && errno == EINTR);
	return ret;
}


static int epoll_deregister( int  epoll_fd, int  fd ) {
	int  ret;
	do {
		ret = epoll_ctl( epoll_fd, EPOLL_CTL_DEL, fd, NULL );
	} while (ret < 0 && errno == EINTR);
	return ret;
}

void update_gps_status(GpsStatusValue val) {
	GpsState*  state = _gps_state;
	//Should be made thread safe...
	state->status.status=val;
	if(state->callbacks.status_cb)
		state->callbacks.status_cb(&state->status);
}

void update_gps_svstatus(GpsSvStatus *val) {
	GpsState*  state = _gps_state;
	//Should be made thread safe...
	if(state->callbacks.sv_status_cb)
		state->callbacks.sv_status_cb(val);
}

/* this is the main thread, it waits for commands from gps_state_start/stop and,
 * when started, messages from the NMEA SMD. these are simple NMEA sentences
 * that must be parsed to be converted into GPS fixes sent to the framework
 */
uint32_t _fix_frequency;//Which is a period not a frequency, but nvm.
static void* gps_state_thread( void*  arg ) {
	GpsState*   state = (GpsState*) arg;
	NmeaReader  reader[1];
	int         epoll_fd   = epoll_create(2);
	int         started    = 0;
	int         gps_fd     = state->fd;
	int         control_fd = state->control[1];

	//Engine is enabled when the thread is started.
	state->status.status=GPS_STATUS_ENGINE_ON;
	if(state->callbacks.status_cb)
		state->callbacks.status_cb(&state->status);
	nmea_reader_init( reader );

	// register control file descriptors for polling
	epoll_register( epoll_fd, control_fd );
	epoll_register( epoll_fd, gps_fd );

	D("gps thread running");

	// now loop
	for (;;) {
		struct epoll_event   events[2];
		int                  ne, nevents;

		nevents = epoll_wait( epoll_fd, events, 2, started ? _fix_frequency*1000 : -1);
		if (nevents < 0) {
			if (errno != EINTR)
				LOGE("epoll_wait() unexpected error: %s", strerror(errno));
			continue;
		}
		if(nevents==0) {
			//We should call pdsm_get_position more often than that... but it's not easy to code.
			//Anyway the 2second timeout is already stupid,
			if(started)
				gps_get_position();
		}
		D("gps thread received %d events", nevents);
		for (ne = 0; ne < nevents; ne++) {
			if ((events[ne].events & (EPOLLERR|EPOLLHUP)) != 0) {
				LOGE("EPOLLERR or EPOLLHUP after epoll_wait() !?");
				goto Exit;
			}
			if ((events[ne].events & EPOLLIN) != 0) {
				int  fd = events[ne].data.fd;

				if (fd == control_fd) {
					char  cmd = 255;
					int   ret;
					D("gps control fd event");
					do {
						ret = read( fd, &cmd, 1 );
					} while (ret < 0 && errno == EINTR);
	
					if (cmd == CMD_QUIT) {
						D("gps thread quitting on demand");
						goto Exit;
					} else if (cmd == CMD_START) {
						if (!started) {
							D("gps thread starting  location_cb=%p", state->callbacks.location_cb);
							started = 1;
						}
					} else if (cmd == CMD_STOP) {
						if (started) {
							D("gps thread stopping");
							started = 0;
							exit_gps_rpc();
						}
 					}
				} else if (fd == gps_fd) {
					char  buff[32];
					D("gps fd event");
					for (;;) {
						int  nn, ret;
	
						ret = read( fd, buff, sizeof(buff) );
						if (ret < 0) {
							if (errno == EINTR)
								continue;
							if (errno != EWOULDBLOCK)
							LOGE("error while reading from gps daemon socket: %s:", strerror(errno));
							break;
						}
						D("received %d bytes: %.*s", ret, ret, buff);
						for (nn = 0; nn < ret; nn++)
							nmea_reader_addc( reader, buff[nn] );
					}
					D("gps fd event end");
				} else {
					LOGE("epoll_wait() returned unkown fd %d ?", fd);
				}
			}
		}
	}
Exit:
    return NULL;
}


static void gps_state_init( GpsState*  state ) {
	state->init       = 1;
	state->control[0] = -1;
	state->control[1] = -1;
	state->fd = open("/dev/smd27", O_RDONLY );

	if (state->fd < 0) {
		D("no gps smd detected(cdma raph/diam ?)");
		return;
	}

	if ( socketpair( AF_LOCAL, SOCK_STREAM, 0, state->control ) < 0 ) {
		LOGE("could not create thread control socket pair: %s", strerror(errno));
		goto Fail;
	}

	if ( pthread_create( &state->thread, NULL, gps_state_thread, state ) != 0 ) {
		LOGE("could not create gps thread: %s", strerror(errno));
		goto Fail;
	}
	if(init_gps_rpc())
		goto Fail;

	D("gps state initialized");
	return;

Fail:
	gps_state_done( state );
}


/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       I N T E R F A C E                               *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/


static int gps_init(GpsCallbacks* callbacks) {
	GpsState*  s = _gps_state;

	if (!s->init)
		gps_state_init(s);

	if (s->fd < 0)
		return -1;

	s->callbacks = *callbacks;

	return 0;
}

static void gps_cleanup() {
	GpsState*  s = _gps_state;

	if (s->init)
		gps_state_done(s);
}


static int gps_start() {
	GpsState*  s = _gps_state;

	if (!s->init) {
		D("%s: called with uninitialized state !!", __FUNCTION__);
		return -1;
	}

	D("%s: called", __FUNCTION__);
	gps_state_start(s);
	return 0;
}


static int gps_stop() {
	GpsState*  s = _gps_state;

	if (!s->init) {
		D("%s: called with uninitialized state !!", __FUNCTION__);
		return -1;
	}

	D("%s: called", __FUNCTION__);
	gps_state_stop(s);
	return 0;
}


static int gps_inject_time(GpsUtcTime time, int64_t timeReference, int uncertainty) {
	return 0;
}

static int gps_inject_location(double latitude, double longitude, float accuracy) {
	return 0;
}

static void gps_delete_aiding_data(GpsAidingData flags) {
}

static int gps_set_position_mode(GpsPositionMode mode, int fix_frequency) {
	_fix_frequency=fix_frequency;
	if(_fix_frequency==0) {
		//We don't handle single shot requests atm...
		//So one every 4seconds will it be.
		_fix_frequency=4;
	}
	if(_fix_frequency>8) {
		//Ok, A9 will timeout with so high value.
		//Set it to 8.
		_fix_frequency=8;
	}
	return 0;
}

static const void* gps_get_extension(const char* name) {
	return NULL;
}

static const GpsInterface  hardwareGpsInterface = {
    gps_init,
    gps_start,
    gps_stop,
    gps_cleanup,
    gps_inject_time,
    gps_inject_location,
    gps_delete_aiding_data,
    gps_set_position_mode,
    gps_get_extension,
};

const GpsInterface* gps_get_hardware_interface()
{
    return &hardwareGpsInterface;
}

