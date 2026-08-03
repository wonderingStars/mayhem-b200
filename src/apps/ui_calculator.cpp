/*
 * mayhem-b200 — RPN calculator (host port of Mayhem's calculator app).
 *
 * The IVT (IVEE-TINY) FORTH-like RPN engine is (C) 2021 zooxo/deetee under the
 * 3-Clause BSD licence (see application/external/calculator/ivt.hpp upstream);
 * transcribed here into an anonymous namespace with host glue in place of the
 * AVR display/keypad/EEPROM. Mayhem wrapper (C) 2023 Bernd Herzog.
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_calculator.hpp"

#include "app_context.hpp"
#include "ui_navigation.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif

namespace app {

/* ===========================================================================
 * IVT virtual machine (verbatim logic; internal linkage).
 * =========================================================================== */
namespace {

using byte = uint8_t;
using boolean = bool;

using std::atan;
using std::cos;
using std::exp;
using std::log;
using std::log10;

// --- Host interface state (was current_key/display_string in the Mayhem wrapper)
byte current_key = 255;
char display_string[10];
byte fgm = 0;
unsigned int mp = 0;

char CHARMAP[] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    'A', 'B', 'C', 'D', 'E', 'F',
    'H', 'I', 'L', 'M', 'N', 'O', 'P', 'R', 'S', 'T', 'U', 'V',
    'c', 's', 't',
    ' ', '.', '*', '+', '-', '/', '!', '<', '=', '>', '^',
    'f', 'n', 'p', 's', 'm', 'w'};

void printcat(byte c, byte x) {  // Print char c at position x
    display_string[x] = CHARMAP[c];
}
byte getkey(void) {  // Read keypad
    return current_key;
}

// --- Font-code indices into CHARMAP (used by cmd[] and printnum)
#define __0 0
#define __1 1
#define __2 2
#define __3 3
#define __4 4
#define __5 5
#define __6 6
#define __7 7
#define __8 8
#define __9 9
#define __A 10
#define __B 11
#define __C 12
#define __D 13
#define __E 14
#define __F 15
#define __H 16
#define __I 17
#define __L 18
#define __M 19
#define __N 20
#define __O 21
#define __P 22
#define __R 23
#define __S 24
#define __T 25
#define __U 26
#define __V 27
#define __c 28
#define __s 29
#define __t 30
#define ___ 31  // space
#define __DOT 32
#define __MULT 33
#define __ADD 34
#define __SUB 35
#define __DIV 36
#define __EM 37  // !
#define __LT 38
#define __EQ 39
#define __GT 40
#define __ARROW 41
#define __FINV 42
#define __POW1 43
#define __PI 44
#define __SQRT 45
#define __MEAN 46
#define __SWAP 47

#define pgm_read_byte(_x) (*(_x))

// --- Forward declarations of the dispatchable functions
static void _keyf(void);
static void _num(void);
static void _e(void);
static void _neg(void);
static void _drop(void);
static void _dot(void);
static void _dup(void);
static void _menu(void);
static void _sumadd(void);
static void _prgedit(void);
static void _div(void);
static void _swap(void);
static void _dict(void);
static void _usrset(void);
static void _mul(void);
static void _rot(void);
static void _mrcl(void);
static void _msto(void);
static void _sub(void);
static void _clr(void);
static void _pi(void);
static void _int(void);
static void _add(void);
static void _condif(void);
static void _condelse(void);
static void _condthen(void);
static void _permcomb(void);
static void _condlt(void);
static void _condeq(void);
static void _condne(void);
static void _condgt(void);
static void _begin(void);
static void _until(void);
static void _solve(void);
static void _inv(void);
static void _cos(void);
static void _atan(void);
static void _exp(void);
static void _ln(void);
static void _sin(void);
static void _tan(void);
static void _asin(void);
static void _acos(void);
static void _over(void);
static void _sqrt(void);
static void _pow(void);
static void _gammaln(void);
static void _pv(void);
static void _nd(void);
static void _pol2rect(void);
static void _rect2pol(void);
static void _sumclr(void);
static void _sumstat(void);
static void _sumlr(void);

static void _numinput(byte k);
static double dpush(double d);
static double dpop(void);
static void seekmem(byte n);
static void apush(int addr);
static int apop(void);
static void _condseek(void);
static int eeudist(byte i);
static void _mstorcl(boolean issto);

template <class T>
void EEwrite(int ee, const T& value);
template <class T>
void EEread(int ee, T& value);

// --- Stubbed EEPROM (matches the Mayhem port: persistence is inert on host)
uint8_t ee_dummy = 0;
struct EEPROM_t {
    void write(uint32_t, uint8_t) {}
    uint8_t read(uint32_t) { return 0; }
    uint8_t& operator[](uint32_t) { return ee_dummy; }
    size_t length() { return 0; }
} EEPROM;

static double pow10(int8_t e) {  // 10 raised to the power of e
    double f = 1.0;
    if (e > 0)
        while (e--) f *= 10.0;
    else
        while (e++) f /= 10.0;
    return f;
}

// --- Number formatting into sbuf / display_string
#define MAXSTRBUF 10
#define DIGITS 8
#define ALMOSTZERO 1e-37
#define FIXMIN 1e-3
#define FIXMAX 1e7
#define PRINTNUMBER 9
#define PRINTMENU 8

static byte sbuf[MAXSTRBUF];
static boolean isnewnumber = true;
static byte decimals = 0;
static boolean isdot = false;

#define _ones(x) ((x) % 10)
#define _tens(x) (((x) / 10) % 10)

static void printsbuf(byte digits) {  // Print "digits" elements of sbuf[]
    for (byte i = 0; i < digits; i++) {
        printcat(sbuf[i], i);
    }
}
static void sbufclr(void) {
    for (byte i = 0; i < sizeof(sbuf); i++) sbuf[i] = ___;
}
static void printnum(double f) {
    int8_t ee = 0;
    int8_t e = 1;
    long m;
    sbufclr();
    if (f < 0.0) {
        f = -f;
        sbuf[0] = __SUB;
    }
    if (f >= ALMOSTZERO && (f < FIXMIN || f >= FIXMAX)) {  // SCI format
        ee = (int8_t)log10(f);
        if (ee < 0) ee--;
        f /= pow10(ee);
    }
    if (f >= 1.0) e = (int8_t)log10(f) + 1;
    double a = pow10(7 - e);
    double d = (f * a + 0.5) / a;
    m = (long)d;
    for (byte i = e; i > 0; i--) {
        sbuf[i] = _ones(m);
        m /= 10;
    }
    sbuf[e + 1] = __DOT;
    if ((long)f >= (long)d) d = f;
    m = (long)((d - (long)d) * a + 0.5);
    boolean istrail = true;
    for (byte i = DIGITS; i > e + 1; i--) {
        byte one = _ones(m);
        if (!istrail || ((isnewnumber || i - e - 1 <= decimals) && (!isnewnumber || one != 0))) {
            sbuf[i] = one;
            istrail = false;
        }
        m /= 10L;
    }
    if (ee) {
        sbuf[6] = (ee < 0) ? __SUB : ___;
        if (ee < 0) ee = -ee;
        sbuf[8] = _ones(ee);
        sbuf[7] = _tens(ee);
    }
    printsbuf(PRINTNUMBER);
}

// --- Keypad
#define PREENDKEY 254
#define ENDKEY 255
static byte key = PREENDKEY, oldkey = PREENDKEY;

static byte key2nr(byte k) {  // Convert key index to digit
    if (k >= 1 && k <= 3)
        return (k + 6);
    else if (k >= 5 && k <= 7)
        return (k - 1);
    else if (k >= 9 && k <= 11)
        return (k - 8);
    else
        return (0);
}

// --- Application constants
#define RAD (180.0 / PI)
#define MAXCMDI 48
#define MAXCMDB 64
#define MAXCMDU 80
#define ISF 1
#define MAXPRG 16
#define MAXPRGBUF 36
#define MEDIMENU 2
#define MEDIDICT 5

#define MEMSTO 10
#define EESTO 0
#define MENUITEMS 32
#define EEMENU 40
#define EEUSTART 72
/* EEPROM length is 0, so there is no user-program memory. Upstream computes
 * EEUEND-EEUSTART = -72 (relying on size_t wraparound); an empty store means 0
 * available user bytes, which we use directly to avoid the underflow. */
#define EEU 0
#define UL0 20

static boolean isprintscreen = true;
static byte setfgm = 0;
static byte select_ = 0;
static byte medi = 0;

static boolean issetusr = false;
static boolean isprgdict = false;
static byte setusrselect = 0;
static boolean isprgedit = false;
static byte prgnr = 0;
static int prgaddr = 0;
static byte prgpos = 0;
static byte prglength = 0;
static byte prgbuf[MAXPRGBUF];

#define DELTAX 1E-4
static byte runs = 0;
static boolean issolve = false;

#define DATASTACKSIZE 24
static double ds[DATASTACKSIZE];
static byte dp = 0;

#define ADDRSTACKSIZE 24
static int as_[ADDRSTACKSIZE];
static byte ap = 0;

static byte cl = 0;  // conditional level

// --- Command codes
#define _7 1
#define _8 2
#define _9 3
#define _4 5
#define _5 6
#define _6 7
#define _NEG 8
#define _1 9
#define _2 10
#define _3 11
#define _DROP 12
#define _0 13
#define _DOT 14
#define _DUP 15
#define _DIV 19
#define _SWAP 20
#define _MULT 23
#define _RCL 25
#define _STO 26
#define _ROT 24
#define _SUB 27
#define _PI 29
#define _ADD 31
#define _IF 32
#define _ELSE 33
#define _THEN 34
#define _LT 36
#define _NE 38
#define _INV 43
#define _BEGIN 40
#define _UNTIL 41
#define _COS 44
#define _ATAN 45
#define _EXP 46
#define _LN 47
#define _SIN (MAXCMDI + 0)
#define _TAN (MAXCMDI + 1)
#define _ASIN (MAXCMDI + 2)
#define _ACOS (MAXCMDI + 3)
#define _OVER (MAXCMDI + 4)
#define _SQRT (MAXCMDI + 5)
#define _POW (MAXCMDI + 6)
#define _PV (MAXCMDI + 7)
#define _SUMADD (MAXCMDI + 8)
#define _SUMCLR (MAXCMDI + 9)
#define _SUMLR (MAXCMDI + 10)
#define _SUMSTAT (MAXCMDI + 11)
#define _GAMMALN (MAXCMDI + 12)
#define _POL2RECT (MAXCMDI + 13)
#define _RECT2POL (MAXCMDI + 14)
#define _ND (MAXCMDI + 15)
#define _PERMCOMB (MAXCMDI + 16)
#define _END 255

// --- Builtin function bytecode
const byte mem[] = {
    _END,
    _9, _0, _SWAP, _SUB, _COS, _END,                                          // 0 SIN
    _DUP, _SIN, _SWAP, _COS, _DIV, _END,                                       // 1 TAN
    _DUP, _MULT, _INV, _1, _SUB, _SQRT, _INV, _ATAN, _END,                     // 2 ASIN
    _DUP, _MULT, _INV, _1, _SUB, _SQRT, _ATAN, _END,                          // 3 ACOS
    _SWAP, _DUP, _ROT, _ROT, _END,                                             // 4 OVER
    _DUP, _0, _NE, _IF, _LN, _2, _DIV, _EXP, _THEN, _END,                      // 5 SQRT
    _SWAP, _LN, _MULT, _EXP, _END,                                             // 6 POW
    _OVER, _1, _ADD, _SWAP, _POW, _DUP, _1, _SUB, _SWAP, _DIV, _SWAP, _DIV, _END,  // 7 PV
    _7, _RCL, _1, _ADD, _7, _STO,                                              // 8 SUMADD
    _DUP, _8, _RCL, _ADD, _8, _STO,
    _DUP, _DUP, _MULT, _5, _RCL, _ADD, _5, _STO,
    _OVER, _MULT, _6, _RCL, _ADD, _6, _STO,
    _9, _RCL, _ADD, _9, _STO, _7, _RCL, _END,
    _0, _DUP, _DUP, _DUP, _DUP, _DUP,                                          // 9 SUMCLR
    _5, _STO, _6, _STO, _7, _STO, _8, _STO, _9, _STO, _END,
    _6, _RCL, _7, _RCL, _MULT, _8, _RCL, _9, _RCL, _MULT, _SUB,                // 10 SUMLR
    _5, _RCL, _7, _RCL, _MULT, _8, _RCL, _DUP, _MULT, _SUB, _DIV,
    _DUP, _8, _RCL, _MULT, _NEG, _9, _RCL, _ADD, _7, _RCL, _DIV, _SWAP, _END,
    _8, _RCL, _7, _RCL, _DIV,                                                  // 11 STAT
    _DUP, _DUP, _MULT, _7, _RCL, _MULT, _NEG, _5, _RCL, _ADD,
    _7, _RCL, _1, _SUB, _DIV, _SQRT, _SWAP, _END,
    _1, _ADD, _DUP, _DUP, _DUP, _DUP, _1, _2, _MULT,                          // 12 GAMMALN
    _SWAP, _1, _0, _MULT, _INV, _SUB, _INV, _ADD, _LN, _1, _SUB, _MULT,
    _SWAP, _LN, _NEG, _2, _PI, _MULT, _LN, _ADD, _2, _DIV, _ADD, _END,
    _DUP, _ROT, _DUP, _COS, _SWAP, _SIN, _ROT, _MULT, _ROT, _ROT, _MULT, _END,  // 13 P>R
    _DUP, _MULT, _SWAP, _DUP, _MULT, _DUP, _ROT, _DUP, _ROT, _ADD, _SQRT,      // 14 R>P
    _ROT, _ROT, _DIV, _SQRT, _ATAN, _SWAP, _END,
    _DUP, _DUP, _DUP, _DUP, _MULT, _MULT, _DOT, _0, _7, _MULT,                 // 15 ND
    _SWAP, _1, _DOT, _6, _MULT, _NEG, _ADD, _EXP, _1, _ADD, _INV, _SWAP,
    _DUP, _MULT, _NEG, _2, _DIV, _EXP, _2, _PI, _MULT, _SQRT, _INV, _MULT, _END,
    _DUP, _ROT, _SWAP,                                                         // 16 PERM COMB
    _OVER, _ROT, _ROT, _SUB, _1, _ROT, _ROT, _SWAP,
    _BEGIN, _SWAP, _ROT, _1, _ROT, _ADD, _DUP, _ROT, _MULT, _SWAP, _ROT,
    _OVER, _OVER, _SWAP, _LT, _UNTIL, _DROP, _DROP,
    _DUP, _ROT, _1, _SWAP,
    _BEGIN, _ROT, _ROT, _DUP, _ROT, _SWAP, _DIV, _ROT, _ROT, _1, _ADD, _SWAP,
    _OVER, _1, _SUB, _OVER, _SWAP, _LT, _UNTIL, _DROP, _DROP, _END};

// --- Command name table (2 font-code chars per command)
static const byte cmd[] = {
    __FINV, ___, ___, __7, ___, __8, ___, __9,
    __E, __E, ___, __4, ___, __5, ___, __6,
    __N, ___, ___, __1, ___, __2, ___, __3,
    __C, ___, ___, __0, ___, __DOT, ___, __D,
    __M, ___, __S, __ADD, __P, __R, ___, __DIV,
    __SWAP, ___, __D, __C, __U, __S, ___, __MULT,
    __R, __T, __R, __C, __S, __T, ___, __SUB,
    __C, __A, __PI, ___, __I, __N, ___, __ADD,
    __I, __F, __E, __L, __T, __H, __P, __C,
    __LT, ___, __EQ, ___, __LT, __GT, ___, __GT,
    __B, __E, __U, __N, __S, __O, __I, ___,
    __c, ___, __t, __POW1, __E, ___, __L, __N,
    __s, ___, __t, ___, __s, __POW1, __c, __POW1,
    __O, __V, __SQRT, ___, __P, ___, __EM, __L,
    __P, __V, __N, __D, __P, __ARROW, __R, __ARROW,
    __S, __ADD, __S, __c, __MEAN, ___, __L, __R,
    __0, __0, __0, __1, __0, __2, __0, __3,
    __0, __4, __0, __5, __0, __6, __0, __7,
    __0, __8, __0, __9, __1, __0, __1, __1,
    __1, __2, __1, __3, __1, __4, __1, __5};

static void (*dispatch[])(void) = {
    &_keyf, &_num, &_num, &_num,
    &_e, &_num, &_num, &_num,
    &_neg, &_num, &_num, &_num,
    &_drop, &_num, &_dot, &_dup,
    &_menu, &_sumadd, &_prgedit, &_div,
    &_swap, &_dict, &_usrset, &_mul,
    &_rot, &_mrcl, &_msto, &_sub,
    &_clr, &_pi, &_int, &_add,
    &_condif, &_condelse, &_condthen, &_permcomb,
    &_condlt, &_condeq, &_condne, &_condgt,
    &_begin, &_until, &_solve, &_inv,
    &_cos, &_atan, &_exp, &_ln,
    &_sin, &_tan, &_asin, &_acos,
    &_over, &_sqrt, &_pow, &_gammaln,
    &_pv, &_nd, &_pol2rect, &_rect2pol,
    &_sumadd, &_sumclr, &_sumstat, &_sumlr};

// --- Function definitions
static void _num(void) { _numinput(key2nr(key)); }
static void _add(void) { dpush(dpop() + dpop()); }
static void _acos(void) { seekmem(_ACOS); }
static void _asin(void) { seekmem(_ASIN); }
static void _atan(void) { dpush(atan(dpop()) * RAD); }
static void _begin(void) { apush(mp); }
static void _ce(void) {
    if (isdot) {
        if (decimals) {
            decimals--;
            double a = pow10(decimals);
            dpush(((long)(dpop() * a) / a));
        } else
            isdot = false;
    } else {
        long a = (long)(dpop() / 10.0);
        if (!a)
            isnewnumber = true;
        else
            dpush(a);
    }
}
static void _clr(void) {
    dp = 0;
    _sumclr();
}
static void _condelse(void) {
    _condseek();
    cl--;
}
static void _condeq(void) { dpush(dpop() == dpop()); }
static void _condgt(void) { dpush(dpop() < dpop()); }
static void _condif(void) {
    cl++;
    if (!dpop()) _condseek();
}
static void _condlt(void) {
    _condgt();
    dpush(!dpop());
}
static void _condne(void) {
    _condeq();
    dpush(!dpop());
}
static void _condseek(void) {
    boolean isloop = true;
    byte cltmp = 0;
    while (isloop) {
        byte c = 0;
        if (mp < sizeof(mem))
            c = pgm_read_byte(mem + mp++);
        else if (mp < sizeof(mem) + EEU)
            c = EEPROM[mp++ - sizeof(mem) + EEUSTART];
        if (mp >= sizeof(mem) + EEU)
            isloop = false;
        else if (c == _IF)
            cltmp++;
        else if (cltmp && c == _THEN)
            cltmp--;
        else if (!cltmp && (c == _ELSE || c == _THEN))
            isloop = false;
    }
}
static void _condthen(void) { cl--; }
static void _cos(void) { dpush(cos(dpop() / RAD)); }
static void _dict(void) {
    select_ = 0;
    medi = MEDIDICT;
}
static void _div(void) {
    _inv();
    _mul();
}
static void _dot(void) {
    if (isnewnumber) {
        dpush(0.0);
        decimals = 0;
        isnewnumber = false;
    }
    isdot = true;
}
static void _drop(void) {
    if (!isnewnumber)
        _ce();
    else if (dp)
        dp--;
}
static void _dup(void) {
    if (isnewnumber && dp) dpush(ds[dp - 1]);
}
static void _e(void) {
    dpush(pow10((int8_t)dpop()));
    _mul();
}
static void _exp(void) {
    boolean isneg = false;
    if (dpush(dpop()) < 0.0) {
        _neg();
        isneg = true;
    }
    dpush(1.0);
    for (byte i = 255; i; i--) {
        _swap();
        _dup();
        _rot();
        dpush(i);
        _div();
        _mul();
        dpush(1.0);
        _add();
    }
    if (isneg) _inv();
    _swap();
    _drop();
}
static void _gammaln(void) { seekmem(_GAMMALN); }
static void _int(void) { dpush((double)(long)dpop()); }
static void _inv(void) { dpush(1.0 / dpop()); }
static void _keyf(void) {
    fgm = ISF;
    setfgm = 0;
}
static void _ln(void) { dpush(log(dpop())); }
static void _menu(void) {
    select_ = 0;
    medi = MEDIMENU;
}
static void _mrcl(void) { _mstorcl(false); }
static void _msto(void) { _mstorcl(true); }
static void _mstorcl(boolean issto) {
    byte nr = (byte)dpop();
    byte addr = EESTO + nr * sizeof(double);
    if (nr < MEMSTO) {
        if (issto)
            EEwrite(addr, dpop());
        else {
            double a;
            EEread(addr, a);
            dpush(a);
        }
    }
}
static void _mul(void) { dpush(dpop() * dpop()); }
static void _nd(void) { seekmem(_ND); }
static void _neg(void) { dpush(-dpop()); }
static void _numinput(byte k) {
    if (isdot) {
        dpush(k);
        dpush(pow10(++decimals));
        _div();
        _add();
    } else if (isnewnumber)
        dpush((double)k);
    else {
        dpush(10.0);
        _mul();
        dpush(k);
        _add();
    }
    isnewnumber = false;
}
static void _over(void) { seekmem(_OVER); }
static void _permcomb(void) { seekmem(_PERMCOMB); }
static void _pi(void) { dpush(PI); }
static void _pol2rect(void) { seekmem(_POL2RECT); }
static void _pow(void) { seekmem(_POW); }
static void _pv(void) { seekmem(_PV); }
static void _prgedit(void) {
    isprgedit = true;
    if ((prgnr = (byte)dpop()) >= MAXPRG) prgnr = 0;
    prgpos = prglength = 0;
    prgaddr = EEUSTART + eeudist(prgnr);
    byte len = prgnr + UL0, prgstep = EEPROM[prgaddr];
    while (prglength < len && prgstep != _END) {
        prgbuf[prglength] = prgstep;
        prgstep = EEPROM[prgaddr + ++prglength];
    }
}
static void _rect2pol(void) { seekmem(_RECT2POL); }
static void _rot(void) {
    if (dp > 2) {
        double a = dpop(), b = dpop(), c = dpop();
        dpush(b);
        dpush(a);
        dpush(c);
    }
}
static void _sin(void) { seekmem(_SIN); }
static void _solve(void) {
    _dup();
    _dup();
    runs = 0;
    issolve = true;
}
static void _sqrt(void) { seekmem(_SQRT); }
static void _sub(void) {
    _neg();
    _add();
}
static void _sumadd(void) { seekmem(_SUMADD); }
static void _sumclr(void) { seekmem(_SUMCLR); }
static void _sumlr(void) { seekmem(_SUMLR); }
static void _sumstat(void) { seekmem(_SUMSTAT); }
static void _swap(void) {
    if (dp > 1) {
        double a = dpop(), b = dpop();
        dpush(a);
        dpush(b);
    }
}
static void _tan(void) { seekmem(_TAN); }
static void _until(void) {
    if (!ap)
        ;
    else if (dpop())
        apop();
    else
        apush(mp = apop());
}
static void _usrset(void) {
    select_ = 0;
    medi = MEDIDICT;
    issetusr = true;
}

static int eeudist(byte i) {
    return ((2 * UL0 + i - 1) * i / 2);
}

static void execute(byte cmd_) {
    if (cmd_ < MAXCMDB)
        (*dispatch[cmd_])();
    else if (cmd_ < MAXCMDU)
        mp = eeudist(cmd_ - MAXCMDB) + sizeof(mem);
    if ((cmd_ % 4 == 0 && cmd_ != 12) || cmd_ > 14) {
        decimals = 0;
        isdot = false;
        isnewnumber = true;
    }
    if (fgm && setfgm) fgm = setfgm = 0;
    setfgm = 1;
}

static void prgstepins(byte c) {
    for (byte i = prglength; i > prgpos; i--) prgbuf[i] = prgbuf[i - 1];
    prgbuf[prgpos + 1] = c;
    prglength++;
    prgpos++;
}

template <class T>
void EEwrite(int ee, const T& value) {
    const byte* p = (const byte*)(const void*)&value;
    for (byte i = 0; i < sizeof(value); i++) EEPROM.write(ee++, *p++);
}
template <class T>
void EEread(int ee, T& value) {
    byte* p = (byte*)(void*)&value;
    for (byte i = 0; i < sizeof(value); i++) *p++ = EEPROM.read(ee++);
}

static void upn(byte n, byte l) {
    if (select_ > n * l && select_ <= (n + 1) * l - 1)
        select_--;
    else
        select_ = (n + 1) * l - 1;
}
static void downn(byte n, byte l) {
    if (select_ >= n * l && select_ < (n + 1) * l - 1)
        select_++;
    else
        select_ = n * l;
}
static byte menuselect(byte lines) {
    char k = key;
    if (k <= 3)
        return (select_ * 4 + k);
    else if (k >= 4 && k <= 7)
        upn(k - 4, lines);
    else if (k >= 8 && k <= 11)
        downn(k - 8, lines);
    if (k == 12) return (ENDKEY);
    if (k == 15) {
        return (ENDKEY);
    }
    return (PREENDKEY);
}

static void floatstack() {
    memmove(ds, &ds[1], (DATASTACKSIZE - 1) * sizeof(double));
    dp--;
}
static double dpush(double d) {
    if (dp >= DATASTACKSIZE) floatstack();
    return (ds[dp++] = d);
}
static double dpop(void) {
    return (dp ? ds[--dp] : 0.0);
}
static void apush(int addr) {
    as_[ap++] = addr;
}
static int apop(void) {
    return (ap ? as_[--ap] : 0);
}
static void seekmem(byte n) {
    mp = 0;
    while (n + 1 - MAXCMDI)
        if (pgm_read_byte(&mem[mp++]) == _END) n--;
}

static boolean printscreen(void) {
    if (medi) {
        for (byte i = 0; i < 4; i++) {
            byte nr = select_ * 4 + i;
            if (medi == MEDIMENU) nr = EEPROM[EEMENU + nr];
            for (byte j = 0; j < 2; j++) sbuf[2 * i + j] = pgm_read_byte(&cmd[2 * nr + j]);
        }
        printsbuf(PRINTMENU);
    } else if (isprgedit) {
        sbuf[0] = ___;
        sbuf[1] = __P;
        sbuf[3] = _ones(prgnr);
        sbuf[2] = _tens(prgnr);
        sbuf[5] = _ones(prgpos);
        sbuf[4] = _tens(prgpos);
        for (byte i = 0; i < 2; i++) sbuf[6 + i] = pgm_read_byte(&cmd[2 * prgbuf[prgpos] + i]);
        printsbuf(PRINTMENU);
    } else if (dp)
        printnum(ds[dp - 1]);
    else
        printcat(__ARROW, 0);

    if (!isnewnumber) printcat(__ARROW, 0);
    if (fgm) printcat(__FINV, 0);
    return (false);
}

static void loop() {
    if (isprintscreen) isprintscreen = printscreen();

    if (mp) {
        if (mp < sizeof(mem))
            key = pgm_read_byte(&mem[mp++]);
        else if (mp < sizeof(mem) + EEU)
            key = EEPROM[mp++ - sizeof(mem) + EEUSTART];
        else
            mp = 0;
        if (key >= MAXCMDI && key != _END) apush(mp);
        if (key == _END) {
            if (ap)
                mp = apop();
            else {
                mp = 0;
                if (!issolve) isprintscreen = true;
            }
        } else
            execute(key);
    } else {
        key = getkey();

        if (issolve) {
            if (++runs < 3) {
                if (runs == 2) {
                    _swap();
                    dpush(DELTAX);
                    _add();
                }
                execute(MAXCMDB);
            } else {
                _swap();
                _div();
                dpush(-1.0);
                _add();
                dpush(DELTAX);
                _swap();
                _div();
                double diffx = dpush(dpop());
                _sub();
                runs = 0;
                if (diffx < DELTAX && diffx > -DELTAX) {
                    isnewnumber = isprintscreen = true;
                    issolve = false;
                } else {
                    _dup();
                    _dup();
                }
            }
        }

        if (key != oldkey) {
            oldkey = key;
            if (key < ENDKEY) {
                if (medi) {
                    byte sel = menuselect(medi);
                    if (sel < PREENDKEY) {
                        if (medi == MEDIMENU) {
                            if (issetusr) {
                                EEPROM[EEMENU + sel] = setusrselect;
                                issetusr = false;
                            } else
                                execute(EEPROM[EEMENU + sel]);
                            medi = 0;
                        } else {
                            if (issetusr) {
                                setusrselect = sel;
                                select_ = 0;
                                medi = MEDIMENU;
                            } else if (isprgdict) {
                                prgstepins(sel);
                                isprgdict = false;
                                isprgedit = true;
                                medi = 0;
                            } else {
                                execute(sel);
                                medi = 0;
                            }
                        }
                    } else if (sel == ENDKEY) {
                        issetusr = false;
                        medi = 0;
                    }
                } else if (isprgedit) {
                    if (key == 0)
                        fgm = fgm ? 0 : ISF;
                    else if (key == 5 && fgm == ISF) {
                        select_ = fgm = 0;
                        medi = MEDIDICT;
                        isprgdict = true;
                        isprgedit = false;
                    } else if (fgm == ISF) {
                        prgstepins(key + fgm * 16);
                        fgm = 0;
                    } else if (key == 4) {
                        if (prgpos) prgpos--;
                    } else if (key == 8) {
                        if (prgpos < prglength - 1) prgpos++;
                    } else if (key == 12) {
                        for (byte i = 0; i < prglength; i++) EEPROM[prgaddr + i] = prgbuf[i];
                        EEPROM[prgaddr + prglength] = _END;
                        isprgedit = false;
                    } else if (key == 14) {
                        if (prglength) {
                            for (byte i = prgpos; i < prglength; i++) prgbuf[i] = prgbuf[i + 1];
                            prglength--;
                        }
                    } else
                        prgstepins(key);
                } else
                    execute(key + fgm * 16);

                isprintscreen = true;
            }
        }
    }
}

static void step() { loop(); }

// --- Reset helper: return the VM to power-on state.
static void vm_reset() {
    current_key = 255;
    fgm = 0;
    mp = 0;
    for (int i = 0; i < 10; i++) display_string[i] = ' ';
    for (int i = 0; i < DATASTACKSIZE; i++) ds[i] = 0.0;
    dp = 0;
    for (int i = 0; i < ADDRSTACKSIZE; i++) as_[i] = 0;
    ap = 0;
    cl = 0;
    isnewnumber = true;
    decimals = 0;
    isdot = false;
    key = oldkey = PREENDKEY;
    isprintscreen = true;
    setfgm = 0;
    select_ = 0;
    medi = 0;
    issetusr = false;
    isprgdict = false;
    setusrselect = 0;
    isprgedit = false;
    prgnr = 0;
    prgaddr = 0;
    prgpos = 0;
    prglength = 0;
    runs = 0;
    issolve = false;
    for (int i = 0; i < MAXPRGBUF; i++) prgbuf[i] = 0;
    sbufclr();
}

// --- Drive one key press to a quiescent state (mirrors on_button_press stepping)
static void vm_press(byte button) {
    for (int i = 0; i < 10; i++) display_string[i] = ' ';
    current_key = button;
    step();
    do {
        current_key = 255;
        step();
    } while (mp);
}

}  // namespace

/* ===========================================================================
 * Testable engine API.
 * =========================================================================== */
namespace calc {

void reset() { vm_reset(); }
void press(uint8_t k) { vm_press(k); }
void press_keys(const uint8_t* keys, size_t count) {
    for (size_t i = 0; i < count; i++) vm_press(keys[i]);
}
double top() { return dp ? ds[dp - 1] : 0.0; }
int depth() { return dp; }
bool fmode() { return fgm != 0; }
std::string display() { return std::string(&display_string[0], 10); }

}  // namespace calc

/* ===========================================================================
 * View.
 * =========================================================================== */

CalculatorView::CalculatorView() {
    add_children({&console, &text_readout,
                  &button_F, &button_7, &button_8, &button_9,
                  &button_E, &button_4, &button_5, &button_6,
                  &button_N, &button_1, &button_2, &button_3,
                  &button_C, &button_0, &button_P, &button_D});

    button_F.on_select = [this](ui::Button&) { on_button_press(0); };
    button_7.on_select = [this](ui::Button&) { on_button_press(1); };
    button_8.on_select = [this](ui::Button&) { on_button_press(2); };
    button_9.on_select = [this](ui::Button&) { on_button_press(3); };
    button_E.on_select = [this](ui::Button&) { on_button_press(4); };
    button_4.on_select = [this](ui::Button&) { on_button_press(5); };
    button_5.on_select = [this](ui::Button&) { on_button_press(6); };
    button_6.on_select = [this](ui::Button&) { on_button_press(7); };
    button_N.on_select = [this](ui::Button&) { on_button_press(8); };
    button_1.on_select = [this](ui::Button&) { on_button_press(9); };
    button_2.on_select = [this](ui::Button&) { on_button_press(10); };
    button_3.on_select = [this](ui::Button&) { on_button_press(11); };
    button_C.on_select = [this](ui::Button&) { on_button_press(12); };
    button_0.on_select = [this](ui::Button&) { on_button_press(13); };
    button_P.on_select = [this](ui::Button&) { on_button_press(14); };
    button_D.on_select = [this](ui::Button&) { on_button_press(15); };
}

void CalculatorView::on_show() {
    View::on_show();
    calc::reset();
    console.clear();
    console.writeln("RPN calculator (IVT)");
    console.writeln("F shifts; F+D=+ F+3=- F+6=*");
    console.writeln("F+9=/  N=+/-  D=enter  C=drop");
    text_readout.set("");
    button_F.focus();
    update_button_labels();
}

void CalculatorView::on_button_press(uint8_t button) {
    const bool pre_fgm = calc::fmode();

    calc::press(button);

    std::string d = calc::display();

    /* Log operations to the tape the way upstream does (the host Console has no
     * carriage-return overwrite, so entry-in-progress is shown live on the
     * readout instead of rewriting a console line). */
    if (pre_fgm && button != 0) {
        static const char* op_names[16] = {
            "", "SUM+", "PRG", "/", "SWAP", "DICT", "USR", "*",
            "ROT", "RCL", "STO", "-", "CA", "PI", "INT", "+"};
        console.writeln(std::string(op_names[button]) + "  " + d);
    } else if (button == 15) {
        console.writeln(d);
    }

    text_readout.set(d);
    update_button_labels();
}

void CalculatorView::update_button_labels() {
    if (calc::fmode()) {
        button_F.set_text("MENU");
        button_7.set_text("SUM+");
        button_8.set_text("PRG");
        button_9.set_text("/");
        button_E.set_text("SWAP");
        button_4.set_text("DICT");
        button_5.set_text("USR");
        button_6.set_text("*");
        button_N.set_text("ROT");
        button_1.set_text("RCL");
        button_2.set_text("STO");
        button_3.set_text("-");
        button_C.set_text("CA");
        button_0.set_text("PI");
        button_P.set_text("INT");
        button_D.set_text("+");
    } else {
        button_F.set_text("F");
        button_7.set_text("7");
        button_8.set_text("8");
        button_9.set_text("9");
        button_E.set_text("E");
        button_4.set_text("4");
        button_5.set_text("5");
        button_6.set_text("6");
        button_N.set_text("N");
        button_1.set_text("1");
        button_2.set_text("2");
        button_3.set_text("3");
        button_C.set_text("C");
        button_0.set_text("0");
        button_P.set_text(".");
        button_D.set_text("D");
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_calculator{{"calculator", "Calculator",
                                     app::Category::Utilities, ui::Color::yellow(),
                                     nullptr,
                                     [] { return std::make_unique<app::CalculatorView>(); }}};
}  // namespace
