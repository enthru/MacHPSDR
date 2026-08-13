#ifndef RIGCTL_H
#define RIGCTL_H

extern void launch_rigctl(RECEIVER *rx);
extern void disable_rigctl(RECEIVER *rx);

extern int launch_serial(RECEIVER *rx);
extern void disable_serial(RECEIVER *rx);

/* Shut both CAT listeners down for good, for a receiver that is about to be
 * freed. Unlike disable_rigctl() this JOINS the network server thread, so on
 * return nothing is left running that holds this receiver.
 *
 * The RIGCTL block itself is deliberately NOT freed: the serial reader blocks
 * in read() with VMIN=1, which no flag and no close can be relied on to
 * interrupt, so its thread may still be sitting on `rigctl` for as long as the
 * port stays silent. It dereferences nothing else of the receiver's (the RX
 * pointer it forwards is re-validated by parse_cmd), so retaining a couple of
 * hundred bytes per closed receiver is the price of not racing a thread that
 * cannot be joined. */
extern void rigctl_close(RECEIVER *rx);

extern int   rigctlGetMode(void);
extern int   lookup_band(int);
extern char * rigctlGetFilter(void);
extern void set_freqB(long long);
extern int set_alc(gpointer);

extern void rigctl_set_debug(RECEIVER *rx);

extern int cat_control;
extern int rigctl_busy;

extern int rigctl_port_base;
extern int rigctl_enable;


#endif // RIGCTL_H
