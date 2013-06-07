/*
 * quad_control.c
 *
 */


#include <limits.h>
#include "quad_control.h"
#include <stdio.h>

/*
#define att_pre_err_div 1
#define att_Kp_div 1
#define att_Kd_div 25
*/
/*
typedef struct { int min, med, max;} triplet;

triplet int_Order3(int a, int b, int c)
{
    triplet retVal;
    if (a < b)
    {
        if (a < c)
        {
            if(b < c)
            {
                retVal.min = a;
                retVal.med = b;
                retVal.max = c;
            }
            else
            {
                retVal.min = a;
                retVal.med = c;
                retVal.max = b;
            }
        }
        else
        {
            retVal.min = c;
            retVal.med = a;
            retVal.max = b;
        }
    }
    else
    {
        if (b < c)
        {
            if (a < c)
            {
                retVal.min = b;
                retVal.med = a;
                retVal.max = c;
            }
            else
            {
                retVal.min = b;
                retVal.med = c;
                retVal.max = a;
            }
        }
        else
        {
            retVal.min = c;
            retVal.med = b;
            retVal.max = a;
        }
    }

    return retVal;
}

int min3(int a, int b, int c)
{
	int min_ab = (a < b)? a: b;
	return (min_ab < c)? min_ab : c;
}


int int_SumSat3(int a, int b, int c)
{
    triplet ordVec = int_Order3(a,b,c);

    if ((ordVec.max = int_SumSat2(ordVec.min, ordVec.max)) == INT_MAX)
        return INT_MAX;
    else
        return int_SumSat2(ordVec.max, ordVec.med);
}
*/

//#define att_Kp 1000
//#define att_Kd 1000

#define c1_gain 1
#define c2_gain 30

vec3 adv_att_control(quat setpoint, quat att)
{
	static vec3 err_prev = VEC0;
	static quat att_prev = UNIT_Q;

	quat setp_c = qconj(setpoint);
	/* Hacia donde me tengo que mover para ir hacia el setpoint*/
	vec3 error = qmul(setp_c, att).v;
	/* Hacia donde me tengo que mover para volver a la posici�n anterior*/
	vec3 damp = qmul(att_prev, qconj(att)).v;
	vec3 torques;
	dvec3 ctrl_signal;
	/*
	ctrl_signal = dvsum(vimul2(error, c1_gain),
					dvsum(vimul2(err_prev, c1_gain),
						dvsum(vimul2(att.v, -c2_gain),
							vimul2(att_prev, c2_gain)
				)));
				*/
	ctrl_signal = dvsum(
					dvsum(vimul2(error, c1_gain),
						  vimul2(err_prev, c1_gain)),
					vimul2(damp, c2_gain)
					);

	//printf("%d %d %d - %d %d %d\n", error.x, error.y, error.z, (att.v.x-att_prev.x), (att.v.y-att_prev.y), (att.v.z-att_prev.z));
	torques = evclip(ctrl_signal);
	
	err_prev = error;
	att_prev = att;
	
	// FIXME: esto esta mal, es para poder probar sin que moleste el yaw 
	torques.z = 0;
	return torques;
}

#define h_Kp_mul 1
#define h_Kd_mul 10

frac h_control(frac setpoint, frac h)
{
	static frac err_prev = 0;
	frac h_error = setpoint - h, thrust;

	thrust = h_error*h_Kp_mul + (h_error - err_prev)*h_Kd_mul;
	err_prev = h_error;

	return thrust;
}

#define mix_thrust_shift 0
#define	mix_roll_shift_r 3
#define mix_pitch_shift_r 3
#define mix_yaw_shift 3

frac gammainv(frac T, frac t1, frac t2, frac t3)
{
	dfrac r = 0;
	
	r += (((dfrac)T) << mix_thrust_shift);
	r += (((dfrac)t1) >> mix_roll_shift_r);
	r += (((dfrac)t2) >> mix_pitch_shift_r);
	r += (((dfrac)t3) << mix_yaw_shift);
	
	return fsqrt((r > 0)? ((r < FRAC_1)? r : FRAC_1) : 0);
	//return ((r > 0)? ((r < FRAC_1)? r : FRAC_1) : 0);
}				

struct motorData control_mixer(frac thrust, vec3 torque)
{
	struct motorData output;
	//VER SIGNO; justo el 0 y el 2 deber�an estar al rev�s con los torques en y.
	output.speed[0] = 0;//gammainv(thrust, 0, torque.y, -torque.z);
	output.speed[1] = gammainv(thrust, -torque.x, 0, torque.z);
	output.speed[2] = 0;//gammainv(thrust, 0, -torque.y, -torque.z);
	output.speed[3] = gammainv(thrust, torque.x, 0, torque.z);
	
	return output;
}
