/*
 * control.c
 * 
 * Referencias:
 * 	R. Mahony, T. Hamel, and J.-M. Pflimlin, “Non-linear complementary 
 * 	filters on the special orthogonal group,” IEEE Trans. Automatic Control,
 * 	vol. 53, no. 5, pp. 1203–1218, June 2008. 
 *
 */
#include "derivative.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include "dmu.h"

#define SAMPLERATE 50 // Hz */

/*esto hay que hacerlo a mano
 * q[n+1] = q[n] + d_q/fs = q[n] + 0.5 * q[n] * p(omega + kp*wmes) / fs
 * machine_omega = real_omega / GYRO_SCALE
 * nos olvidamos del wmes por ahora
 *  =>
 * q[n+1] = q[n] + 0.5 * q[n] * p[machine_omega] * GYRO_SCALE / fs
 * Comprimiendo las constantes:
 * q[n+1] = q[n] + q[n] * p(machine_omega) / D_Q_SCALE
 * 	donde D_Q_SCALE = 1/(0.5 * GYRO_SCALE / fs) = 2*fs / GYRO_SCALE
 */
#define GYRO_SCALE 34.90658504 /* rad / s @ fsd ; son 5.125 bits */
#define D_Q_SCALE 3 /*en realidad es 2.864788976, o 1.518 */

/* esto también va a mano
 *	machine_wmes = wmes / ACC_SCALE
 * wmes va sumado al gyro, por lo tanto:
 * 	v = omega + kp*wmes
 * 	machine_v = machine_omega + machine_wmes/WMES_DIV
 * 	machine_v = v / GYRO_SCALE = real_omega / GYRO_SCALE + kp*wmes / GYRO_SCALE
 *  =>
 * 	kp*wmes/GYRO_SCALE = kp*machine_wmes*ACC_SCALE/GYRO_SCALE = machine_wmes/ WMES_DIV
 * WMES_DIV = GYRO_SCALE / (kp * ACC_SCALE)
 */
#define ACC_SCALE (16*9.81) /* = 156.96 = 16g m*s^2 , son 7.29 bits */
#define Kp (1)
#define WMES_DIV 0.2223 /* debiera ser 0.2223 o -2.16 bits */
#define WMES_MUL 4 /* debiera ser 1/WMES_DIV, o 4.497 */

/* Con el bias hay 2 grados de libertad
 *  (1) bias en sí, que depende de wmes.
 *  (2) bias con respecto a omega
 * La clave está en saber en que rango puede estar bias para aprovechar todo
 * el rango dinámico (1), y a partir de eso determinar (2)
 * MPU-6000 Zero rate output (ZRO): +- 20 °/s = 0.35 rad / s. Empirico: 0.1 max
 * 
 * machine_bias = bias / BIAS_SCALE
 * 
 * (1)
 * 	bias[n+1] = bias[n] + Ki * wmes / fs
 * 	machine_bias[n+1] = bias[n+1] / BIAS_SCALE 
 * 			= bias[n]/BIAS_SCALE + Ki * wmes / (BIAS_SCALE * fs)
 * 				= machine_bias + machine_wmes / D_BIAS_SCALE
 * machine_wmes = 
 * 	=>
 * 	Ki*wmes/(BIAS_SCALE*fs) = machine_wmes / D_BIAS_SCALE
 * 	Ki*(machine_wmes * ACC_SCALE)/(BIAS_SCALE*fs) = machine_wmes / D_BIAS_SCALE
 * 	D_BIAS_SCALE = BIAS_SCALE*fs / (ACC_SCALE * Ki)
 * 
 * (2)
 *	omega_corrected = omega + bias
 * 	machine_omega_corrected = omega_corrected / GYRO_SCALE
 * 	omega/GYRO_SCALE + bias/GYRO_SCALE = machine_omega + machine_bias / BIAS_SCALE2
 * 	machine_bias * BIAS_SCALE / GYRO_SCALE = machine_bias / BIAS_SCALE2;
 * 		=> BIAS_SCALE2 = GYRO_SCALE / BIAS_SCALE
 * 
 * Solución 1: BIAS_SCALE = ZRO (empirico) = 0.1
 *		BIAS_SCALE2 = 349
 * 		D_BIAS_SCALE = 5/(156*0.3) = 0.106
 * 	Problema: en una multiplico y en la otra divido
 * Solucion 2: BIAS_SCALE = 2
 * 		BIAS_SCALE2 = 17
 * 		D_BIAS_SCALE = 2
 * 
 * Nota: El bias se suma para no tener que escribir una función de sustraccion
 * de vectores.
 */
#define Ki 0.3
#define D_BIAS_SCALE 2
#define BIAS_SCALE2 17 /* GYRO_SCALE / BIAS_SCALE */

#include "arith.h"

/* Non-linear complementary filter on SO(3) */

#define OPT_INLINE /*nada */

static OPT_INLINE vec3 z_dir(quat q)
{
	vec3 zd;
	frac tmp;
	
	zd.x = 2*(-fmul(q.v.y, q.r) + fmul(q.v.z, q.v.x));
	zd.y = 2*(fmul(q.v.z, q.v.y) + fmul(q.v.x, q.r));
	tmp = -fmul(q.v.x, q.v.x) - fmul(q.v.y, q.v.y);
	zd.z = FRAC_1 + tmp + tmp;
	
	return zd;
}


static OPT_INLINE vec3 verror(vec3 x, vec3 y)
{
	vec3 zd;
	
	zd.x = fmul(y.z, x.y) - fmul(x.z, y.y);
	zd.y = fmul(y.x, x.z) - fmul(x.x, y.z);
	zd.z = fmul(y.y, x.x) - fmul(x.y, y.x);
	
	return zd;
}

#define FRAC2DBL(n) (((double)(n))/(-(double)FRAC_minus1))
quat att_estim(vec3 gyro, vec3 accel)
{
	static dquat q = {DFRAC_1, {0, 0, 0}}; // UNIT_DQ;
	static vec3 bias = {0, 0, 0};
	vec3 z_estim, wmes, d_bias;
	quat p;
	quat q_lowres = qtrunc(q);
	dquat d_q, correction;
	frac err;
	
	// wmes, bias 
	z_estim = z_dir(q_lowres);

#ifdef PRINTZ

	printf("%g %g %g\n",	FRAC2DBL(z_estim.x),
				FRAC2DBL(z_estim.y),
				FRAC2DBL(z_estim.z));
#endif //PRINTZ

	wmes = verror(accel, z_estim);

#ifdef PRINTW
	printf("%g %g %g\n",	FRAC2DBL(wmes.x),
				FRAC2DBL(wmes.y),
				FRAC2DBL(wmes.z));
#endif // PRINTW
	
	d_bias = vec_Div(wmes, D_BIAS_SCALE);

#ifdef PRINTB
	printf("%g %g %g\n",	FRAC2DBL(bias.x),
				FRAC2DBL(bias.y),
				FRAC2DBL(bias.z));
#endif //PRINTB
	
	// d_q 
	p.r = 0;
	p.v = vec_Add(vec_Add(gyro, vec_Div(bias, BIAS_SCALE2)), vec_Mul(wmes, WMES_MUL));
	//p.v = vec_Add(gyro, vec_Div(wmes, WMES_DIV));
	
	d_q = qmul2(q_lowres, p, D_Q_SCALE);
	
	// bias, q 
	bias = vec_Add(bias, d_bias);
	q = dqsum(q, d_q);
	
	// renormalización 
	err = q_normerror(qtrunc(q));
	correction = qscale2(q_lowres, err);
	q = dqsum(q, correction);
	// q = dqsum(q, d_q);
	
	return qtrunc(q);
}

quat QEst;

void att_process(void)
{
	PORTA_PA0 = 1;
	{
		vec3 acc, gy;
	
		acc.x = dmu_measurements.accel.x;
		acc.y = dmu_measurements.accel.y;
		acc.z =	dmu_measurements.accel.z;
	
		gy.x = dmu_measurements.gyro.x;
		gy.y = dmu_measurements.gyro.y;
		gy.z = dmu_measurements.gyro.z;
	
//		QEst = att_estim(gy, acc);
		printf("%d %d %d %d,", QEst.r, QEst.v.x, QEst.v.y, QEst.v.z);	
	}
	PORTA_PA0 = 0;
}

#if 0
int main(int argc, char *argv[])
{
	while (1) {
		int n;
		//float ax, ay, az, gx, gy, gz;
		vec3 gyro;
		vec3 acc;
		quat q;

		n = scanf("%hd %hd %hd %hd %hd %hd\n", &acc.x, &acc.y, &acc.z, 
						&gyro.x, &gyro.y, &gyro.z);
		
		if (n != 6)
			break;
		
#define F_TO_FRAC(f) ((f) / (1 << 15))

		q = att_estim(gyro, acc);

#ifdef PRINTQ
		printf("%g, %g, %g, %g\n", 
				FRAC2DBL(q.r),
				FRAC2DBL(q.v.x),
				FRAC2DBL(q.v.y),
				FRAC2DBL(q.v.z));
#endif //PRINTQ
	}
	
	return 0;
}
#endif
