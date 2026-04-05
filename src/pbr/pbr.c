/*
 * PBR.C -- PBR-lite Shader Plugin for LightWave 3D
 *
 * Combines Fresnel reflection, roughness (normal perturbation),
 * ambient occlusion (ray-based), and metallic mode into a single
 * physically-based shader for LightWave 5.x on AmigaOS.
 *
 * Uses AllocMem/FreeMem and custom helpers — no libnix runtime.
 */

#include <splug.h>
#include <lwran.h>
#include <lwpanel.h>
#include <lwmath.h>

#include <string.h>

#include <proto/exec.h>
#include <exec/memory.h>

extern struct ExecBase *SysBase;
extern double sqrt(double);

/* ----------------------------------------------------------------
 * Memory helpers
 * ---------------------------------------------------------------- */

static void *
plugin_alloc(unsigned long size)
{
	unsigned long *p;
	p = (unsigned long *)AllocMem(size + 4, MEMF_PUBLIC | MEMF_CLEAR);
	if (!p) return 0;
	*p = size + 4;
	return (void *)(p + 1);
}

static void
plugin_free(void *ptr)
{
	unsigned long *p;
	if (!ptr) return;
	p = ((unsigned long *)ptr) - 1;
	FreeMem(p, *p);
}

/* ----------------------------------------------------------------
 * Integer/string helpers
 * ---------------------------------------------------------------- */

static void
int_to_str(int val, char *buf, int buflen)
{
	char tmp[12];
	int  i = 0, neg = 0, len;

	if (val < 0) { neg = 1; val = -val; }
	if (val == 0) { tmp[i++] = '0'; }
	else {
		while (val > 0 && i < 11) {
			tmp[i++] = (char)('0' + (val % 10));
			val /= 10;
		}
	}
	len = neg + i;
	if (len >= buflen) len = buflen - 1;
	if (neg) buf[0] = '-';
	{
		int j;
		for (j = 0; j < i && (neg + j) < buflen - 1; j++)
			buf[neg + j] = tmp[i - 1 - j];
	}
	buf[len] = '\0';
}

static int
str_to_int(const char *s)
{
	int val = 0, neg = 0;
	while (*s == ' ') s++;
	if (*s == '-') { neg = 1; s++; }
	while (*s >= '0' && *s <= '9') {
		val = val * 10 + (*s - '0');
		s++;
	}
	return neg ? -val : val;
}

/* ----------------------------------------------------------------
 * Math helpers
 * ---------------------------------------------------------------- */

static double
pow_int(double base, int exp)
{
	double result = 1.0;
	int i;
	if (exp < 0) return 0.0;
	for (i = 0; i < exp; i++)
		result *= base;
	return result;
}

static void
vec_normalize(double v[3])
{
	double len = sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
	if (len > 0.00001) {
		v[0] /= len;
		v[1] /= len;
		v[2] /= len;
	}
}

/*
 * Deterministic 3D hash for roughness perturbation.
 * Returns a value in -1.0 to 1.0 range.
 */
static double
hash3d(double x, double y, double z, unsigned int seed)
{
	unsigned int ix = (unsigned int)((x + 1000.0) * 731.0) & 0xFFFFu;
	unsigned int iy = (unsigned int)((y + 1000.0) * 541.0) & 0xFFFFu;
	unsigned int iz = (unsigned int)((z + 1000.0) * 379.0) & 0xFFFFu;
	unsigned int h = ix * 73856093u ^ iy * 19349669u ^ iz * 83492791u ^ seed;
	h = (h >> 13) ^ h;
	h = h * (h * 15731u + 789221u) + 1376312589u;
	return ((double)(h & 0x7FFFu) / (double)0x3FFFu) - 1.0;
}

/* ----------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------- */

typedef struct {
	/* Fresnel */
	double ior;
	int    reflPower;
	int    affectMirror;
	int    affectTrans;
	int    affectDiffuse;
	int    diffPower;
	double f0;

	/* Roughness */
	int    roughEnabled;
	int    roughAmount;      /* 0-100, used as percentage */

	/* Ambient Occlusion */
	int    aoEnabled;
	int    aoSamples;        /* 4, 8, or 16 */
	int    aoRadius;         /* stored as radius * 100 (centimeters) */
	int    aoStrength;       /* 0-100 */

	/* Metallic */
	int    metallic;
} PBRInst;

/* ----------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------- */

static MessageFuncs *msg;

/* ----------------------------------------------------------------
 * Precompute F0 from IOR
 * ---------------------------------------------------------------- */

static void
compute_f0(PBRInst *inst)
{
	double r;
	if (inst->ior < 1.0) inst->ior = 1.0;
	r = (inst->ior - 1.0) / (inst->ior + 1.0);
	inst->f0 = r * r;
}

/* ----------------------------------------------------------------
 * AO hemisphere sample directions (pre-normalized)
 * 6 axis + 8 diagonals + 2 extras = 16 total
 * ---------------------------------------------------------------- */

static const double ao_dirs[16][3] = {
	{ 0.000,  1.000,  0.000},
	{ 1.000,  0.000,  0.000},
	{-1.000,  0.000,  0.000},
	{ 0.000,  0.000,  1.000},
	{ 0.000,  0.000, -1.000},
	{ 0.000, -1.000,  0.000},
	{ 0.577,  0.577,  0.577},
	{-0.577,  0.577,  0.577},
	{ 0.577, -0.577,  0.577},
	{ 0.577,  0.577, -0.577},
	{-0.577, -0.577,  0.577},
	{-0.577,  0.577, -0.577},
	{ 0.577, -0.577, -0.577},
	{-0.577, -0.577, -0.577},
	{ 0.707,  0.707,  0.000},
	{ 0.000,  0.707,  0.707}
};

/* ----------------------------------------------------------------
 * Handler callbacks
 * ---------------------------------------------------------------- */

XCALL_(static LWInstance)
Create(LWError *err)
{
	PBRInst *inst;
	XCALL_INIT;

	inst = (PBRInst *)plugin_alloc(sizeof(PBRInst));
	if (!inst) return 0;

	inst->ior           = 1.5;
	inst->reflPower     = 5;
	inst->affectMirror  = 1;
	inst->affectTrans   = 1;
	inst->affectDiffuse = 1;
	inst->diffPower     = 5;
	inst->roughEnabled  = 0;
	inst->roughAmount   = 20;
	inst->aoEnabled     = 0;
	inst->aoSamples     = 8;
	inst->aoRadius      = 100;
	inst->aoStrength    = 50;
	inst->metallic      = 0;
	compute_f0(inst);

	return inst;
}

XCALL_(static void)
Destroy(PBRInst *inst)
{
	XCALL_INIT;
	if (inst) plugin_free(inst);
}

XCALL_(static LWError)
Copy(PBRInst *from, PBRInst *to)
{
	XCALL_INIT;
	*to = *from;
	return 0;
}

XCALL_(static LWError)
Load(PBRInst *inst, const LWLoadState *ls)
{
	char buf[32];
	XCALL_INIT;

	if (ls->ioMode != LWIO_SCENE)
		return 0;

	buf[0] = '\0';
	(*ls->read)(ls->readData, buf, 32);
	if (buf[0] == '\0')
		(*ls->read)(ls->readData, buf, 32);
	if (buf[0]) inst->ior = str_to_int(buf) / 1000.0;

	buf[0] = '\0'; (*ls->read)(ls->readData, buf, 32);
	if (buf[0]) inst->reflPower = str_to_int(buf);

	buf[0] = '\0'; (*ls->read)(ls->readData, buf, 32);
	if (buf[0]) inst->affectMirror = str_to_int(buf);

	buf[0] = '\0'; (*ls->read)(ls->readData, buf, 32);
	if (buf[0]) inst->affectTrans = str_to_int(buf);

	buf[0] = '\0'; (*ls->read)(ls->readData, buf, 32);
	if (buf[0]) inst->affectDiffuse = str_to_int(buf);

	buf[0] = '\0'; (*ls->read)(ls->readData, buf, 32);
	if (buf[0]) inst->diffPower = str_to_int(buf);

	buf[0] = '\0'; (*ls->read)(ls->readData, buf, 32);
	if (buf[0]) inst->roughEnabled = str_to_int(buf);

	buf[0] = '\0'; (*ls->read)(ls->readData, buf, 32);
	if (buf[0]) inst->roughAmount = str_to_int(buf);

	buf[0] = '\0'; (*ls->read)(ls->readData, buf, 32);
	if (buf[0]) inst->aoEnabled = str_to_int(buf);

	buf[0] = '\0'; (*ls->read)(ls->readData, buf, 32);
	if (buf[0]) inst->aoSamples = str_to_int(buf);

	buf[0] = '\0'; (*ls->read)(ls->readData, buf, 32);
	if (buf[0]) inst->aoRadius = str_to_int(buf);

	buf[0] = '\0'; (*ls->read)(ls->readData, buf, 32);
	if (buf[0]) inst->aoStrength = str_to_int(buf);

	buf[0] = '\0'; (*ls->read)(ls->readData, buf, 32);
	if (buf[0]) inst->metallic = str_to_int(buf);

	compute_f0(inst);
	return 0;
}

XCALL_(static LWError)
Save(PBRInst *inst, const LWSaveState *ss)
{
	char buf[32];
	XCALL_INIT;

	if (ss->ioMode != LWIO_SCENE)
		return 0;

	int_to_str((int)(inst->ior * 1000.0), buf, 32);
	(*ss->write)(ss->writeData, buf, strlen(buf));

	int_to_str(inst->reflPower, buf, 32);
	(*ss->write)(ss->writeData, buf, strlen(buf));

	int_to_str(inst->affectMirror, buf, 32);
	(*ss->write)(ss->writeData, buf, strlen(buf));

	int_to_str(inst->affectTrans, buf, 32);
	(*ss->write)(ss->writeData, buf, strlen(buf));

	int_to_str(inst->affectDiffuse, buf, 32);
	(*ss->write)(ss->writeData, buf, strlen(buf));

	int_to_str(inst->diffPower, buf, 32);
	(*ss->write)(ss->writeData, buf, strlen(buf));

	int_to_str(inst->roughEnabled, buf, 32);
	(*ss->write)(ss->writeData, buf, strlen(buf));

	int_to_str(inst->roughAmount, buf, 32);
	(*ss->write)(ss->writeData, buf, strlen(buf));

	int_to_str(inst->aoEnabled, buf, 32);
	(*ss->write)(ss->writeData, buf, strlen(buf));

	int_to_str(inst->aoSamples, buf, 32);
	(*ss->write)(ss->writeData, buf, strlen(buf));

	int_to_str(inst->aoRadius, buf, 32);
	(*ss->write)(ss->writeData, buf, strlen(buf));

	int_to_str(inst->aoStrength, buf, 32);
	(*ss->write)(ss->writeData, buf, strlen(buf));

	int_to_str(inst->metallic, buf, 32);
	(*ss->write)(ss->writeData, buf, strlen(buf));

	return 0;
}

XCALL_(static LWError)
Init(PBRInst *inst)
{
	XCALL_INIT;
	compute_f0(inst);
	return 0;
}

XCALL_(static void)
Cleanup(PBRInst *inst) { XCALL_INIT; }

XCALL_(static LWError)
NewTime(PBRInst *inst, LWFrame f, LWTime t)
{
	XCALL_INIT;
	return 0;
}

XCALL_(static unsigned int)
Flags(PBRInst *inst)
{
	unsigned int f = 0;
	XCALL_INIT;

	f |= LWSHF_NORMAL;

	if (inst->affectMirror)  f |= LWSHF_MIRROR;
	if (inst->affectTrans)   f |= LWSHF_TRANSP;
	if (inst->affectDiffuse) f |= LWSHF_DIFFUSE;

	if (inst->metallic)
		f |= LWSHF_COLOR | LWSHF_DIFFUSE | LWSHF_SPECULAR;

	if (inst->aoEnabled)
		f |= LWSHF_DIFFUSE | LWSHF_LUMINOUS | LWSHF_RAYTRACE;

	return f;
}

XCALL_(static void)
Evaluate(PBRInst *inst, ShaderAccess *sa)
{
	double cosAngle, oneMinusCos, fresnel;

	XCALL_INIT;

	cosAngle = sa->cosine;
	if (cosAngle < 0.0) cosAngle = -cosAngle;
	if (cosAngle > 1.0) cosAngle = 1.0;
	oneMinusCos = 1.0 - cosAngle;

	/* --- Roughness: perturb surface normal --- */
	if (inst->roughEnabled && inst->roughAmount > 0) {
		double scale = inst->roughAmount / 1000.0;
		double nx = hash3d(sa->oPos[0], sa->oPos[1], sa->oPos[2], 0u) * scale;
		double ny = hash3d(sa->oPos[0], sa->oPos[1], sa->oPos[2], 7919u) * scale;
		double nz = hash3d(sa->oPos[0], sa->oPos[1], sa->oPos[2], 15737u) * scale;

		sa->wNorm[0] += nx;
		sa->wNorm[1] += ny;
		sa->wNorm[2] += nz;
		vec_normalize(sa->wNorm);
	}

	/* --- Metallic: boost F0, reduce diffuse, tint specular --- */
	if (inst->metallic) {
		fresnel = 0.5 + 0.5 * pow_int(oneMinusCos, inst->reflPower);
		if (fresnel > 1.0) fresnel = 1.0;

		sa->mirror = sa->mirror + (1.0 - sa->mirror) * fresnel;
		sa->diffuse *= 0.05;
		sa->specular = sa->specular + (1.0 - sa->specular) * fresnel;
	} else {
		/* --- Fresnel: Schlick's approximation --- */
		if (inst->affectMirror || inst->affectTrans) {
			fresnel = inst->f0 + (1.0 - inst->f0)
			          * pow_int(oneMinusCos, inst->reflPower);
			if (fresnel > 1.0) fresnel = 1.0;
			if (fresnel < 0.0) fresnel = 0.0;

			if (inst->affectMirror)
				sa->mirror = sa->mirror
				             + (1.0 - sa->mirror) * fresnel;
			if (inst->affectTrans)
				sa->transparency *= (1.0 - fresnel);
		}

		if (inst->affectDiffuse) {
			double diffFresnel = inst->f0 + (1.0 - inst->f0)
			                     * pow_int(oneMinusCos, inst->diffPower);
			if (diffFresnel > 1.0) diffFresnel = 1.0;
			if (diffFresnel < 0.0) diffFresnel = 0.0;
			sa->diffuse *= (1.0 - diffFresnel);
		}
	}

	/* --- Ambient Occlusion: cast rays in hemisphere --- */
	if (inst->aoEnabled && inst->aoSamples > 0 && sa->rayCast) {
		double aoRadius = inst->aoRadius / 100.0;
		double aoStr = inst->aoStrength / 100.0;
		int    nSamples = inst->aoSamples;
		int    hits = 0;
		int    tested = 0;
		int    i;
		double pos[3], dir[3], dot, dist;

		/* Offset ray origin slightly along normal to avoid self-hit */
		pos[0] = sa->wPos[0] + sa->wNorm[0] * 0.001;
		pos[1] = sa->wPos[1] + sa->wNorm[1] * 0.001;
		pos[2] = sa->wPos[2] + sa->wNorm[2] * 0.001;

		if (nSamples > 16) nSamples = 16;

		for (i = 0; i < nSamples; i++) {
			dir[0] = ao_dirs[i][0];
			dir[1] = ao_dirs[i][1];
			dir[2] = ao_dirs[i][2];

			/* Only cast rays into the hemisphere facing the normal */
			dot = dir[0]*sa->wNorm[0] + dir[1]*sa->wNorm[1]
			    + dir[2]*sa->wNorm[2];
			if (dot <= 0.0) {
				/* Flip direction to face the right hemisphere */
				dir[0] = -dir[0];
				dir[1] = -dir[1];
				dir[2] = -dir[2];
			}

			dist = (*sa->rayCast)(pos, dir);
			tested++;

			if (dist > 0.0 && dist < aoRadius)
				hits++;
		}

		if (tested > 0) {
			double occlusion = (double)hits / (double)tested;
			double aoFactor = 1.0 - (occlusion * aoStr);
			if (aoFactor < 0.0) aoFactor = 0.0;

			sa->diffuse *= aoFactor;
			sa->luminous *= aoFactor;
		}
	}
}

/* ----------------------------------------------------------------
 * Interface
 * ---------------------------------------------------------------- */

static const char *aoSampleItems[] = { "4", "8", "16", 0 };
static int aoSampleValues[] = { 4, 8, 16 };

XCALL_(static int)
Interface(
	long       version,
	GlobalFunc *global,
	PBRInst    *inst,
	void       *serverData)
{
	LWPanelFuncs *panl;
	LWPanelID     pan;
	LWControl    *ctlIOR, *ctlReflPow, *ctlMirror, *ctlTrans;
	LWControl    *ctlDiffuse, *ctlDiffPow, *ctlMetallic;
	LWControl    *ctlRoughEn, *ctlRoughAmt;
	LWControl    *ctlAOEn, *ctlAOSamp, *ctlAORadius, *ctlAOStr;
	int           aoIdx;
	char          infoBuf[80];

	XCALL_INIT;
	if (version != 1)
		return AFUNC_BADVERSION;

	msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	panl = (LWPanelFuncs *)(*global)(PANEL_SERVICES_NAME, GFUSE_TRANSIENT);

	if (panl) {
		static LWPanControlDesc desc;
		static LWValue ival = {LWT_INTEGER};
		static LWValue fval = {LWT_FLOAT};
		(void)fval;

		pan = PAN_CREATE(panl, "PBR Shader");
		if (!pan) goto fallback;

		/* Fresnel section */
		ctlIOR      = FLOAT_CTL(panl, pan, "Index of Refraction");
		ctlMetallic = BOOL_CTL(panl, pan, "Metallic");
		ctlMirror   = BOOL_CTL(panl, pan, "Affect Reflection");
		ctlReflPow  = SLIDER_CTL(panl, pan, "Reflection Power", 150, 1, 10);
		ctlTrans    = BOOL_CTL(panl, pan, "Affect Transparency");
		ctlDiffuse  = BOOL_CTL(panl, pan, "Affect Diffuse");
		ctlDiffPow  = SLIDER_CTL(panl, pan, "Diffuse Power", 150, 1, 10);

		/* Roughness section */
		ctlRoughEn  = BOOL_CTL(panl, pan, "Enable Roughness");
		ctlRoughAmt = SLIDER_CTL(panl, pan, "Roughness Amount", 150, 0, 100);

		/* AO section */
		ctlAOEn     = BOOL_CTL(panl, pan, "Enable Ambient Occlusion");
		ctlAOSamp   = POPUP_CTL(panl, pan, "AO Samples", aoSampleItems);
		ctlAORadius = FLOAT_CTL(panl, pan, "AO Radius (m)");
		ctlAOStr    = SLIDER_CTL(panl, pan, "AO Strength", 150, 0, 100);

		/* Set values */
		SET_FLOAT(ctlIOR, inst->ior);
		SET_INT(ctlMetallic, inst->metallic);
		SET_INT(ctlMirror, inst->affectMirror);
		SET_INT(ctlReflPow, inst->reflPower);
		SET_INT(ctlTrans, inst->affectTrans);
		SET_INT(ctlDiffuse, inst->affectDiffuse);
		SET_INT(ctlDiffPow, inst->diffPower);
		SET_INT(ctlRoughEn, inst->roughEnabled);
		SET_INT(ctlRoughAmt, inst->roughAmount);
		SET_INT(ctlAOEn, inst->aoEnabled);
		aoIdx = (inst->aoSamples <= 4) ? 0 : (inst->aoSamples <= 8) ? 1 : 2;
		SET_INT(ctlAOSamp, aoIdx);
		{
			double r = inst->aoRadius / 100.0;
			SET_FLOAT(ctlAORadius, r);
		}
		SET_INT(ctlAOStr, inst->aoStrength);

		if (PAN_POST(panl, pan)) {
			GET_FLOAT(ctlIOR, inst->ior);
			GET_INT(ctlMetallic, inst->metallic);
			GET_INT(ctlMirror, inst->affectMirror);
			GET_INT(ctlReflPow, inst->reflPower);
			GET_INT(ctlTrans, inst->affectTrans);
			GET_INT(ctlDiffuse, inst->affectDiffuse);
			GET_INT(ctlDiffPow, inst->diffPower);
			GET_INT(ctlRoughEn, inst->roughEnabled);
			GET_INT(ctlRoughAmt, inst->roughAmount);
			GET_INT(ctlAOEn, inst->aoEnabled);
			GET_INT(ctlAOSamp, aoIdx);
			inst->aoSamples = (aoIdx < 3) ? aoSampleValues[aoIdx] : 8;
			{
				double r;
				GET_FLOAT(ctlAORadius, r);
				inst->aoRadius = (int)(r * 100.0);
			}
			GET_INT(ctlAOStr, inst->aoStrength);

			/* Clamp */
			if (inst->ior < 1.0) inst->ior = 1.0;
			if (inst->ior > 5.0) inst->ior = 5.0;
			if (inst->reflPower < 1) inst->reflPower = 1;
			if (inst->reflPower > 10) inst->reflPower = 10;
			if (inst->diffPower < 1) inst->diffPower = 1;
			if (inst->diffPower > 10) inst->diffPower = 10;
			if (inst->roughAmount < 0) inst->roughAmount = 0;
			if (inst->roughAmount > 100) inst->roughAmount = 100;
			if (inst->aoRadius < 1) inst->aoRadius = 1;
			if (inst->aoStrength < 0) inst->aoStrength = 0;
			if (inst->aoStrength > 100) inst->aoStrength = 100;

			compute_f0(inst);
		}

		PAN_KILL(panl, pan);
		return AFUNC_OK;
	}

fallback:
	if (!msg)
		return AFUNC_BADGLOBAL;

	{
		char nb[12];
		int iorFixed = (int)(inst->ior * 100.0);
		strcpy(infoBuf, "IOR:");
		int_to_str(iorFixed / 100, nb, 12); strcat(infoBuf, nb);
		strcat(infoBuf, ".");
		int_to_str(iorFixed % 100, nb, 12);
		if (iorFixed % 100 < 10) strcat(infoBuf, "0");
		strcat(infoBuf, nb);
		if (inst->metallic) strcat(infoBuf, " Metal");
		if (inst->roughEnabled) {
			strcat(infoBuf, " Rough:");
			int_to_str(inst->roughAmount, nb, 12);
			strcat(infoBuf, nb);
		}
		if (inst->aoEnabled) strcat(infoBuf, " +AO");
	}
	(*msg->info)("PBR Shader", infoBuf);
	return AFUNC_OK;
}

/* ----------------------------------------------------------------
 * Activation
 * ---------------------------------------------------------------- */

XCALL_(int)
Activate(
	long         version,
	GlobalFunc  *global,
	void        *local,
	void        *serverData)
{
	ShaderHandler *h = (ShaderHandler *)local;
	XCALL_INIT;

	if (version < 1)
		return AFUNC_BADVERSION;

	h->create   = (void *)Create;
	h->destroy  = (void *)Destroy;
	h->load     = (void *)Load;
	h->save     = (void *)Save;
	h->copy     = (void *)Copy;
	h->init     = (void *)Init;
	h->cleanup  = (void *)Cleanup;
	h->newTime  = (void *)NewTime;
	h->evaluate = (void *)Evaluate;
	h->flags    = (void *)Flags;

	msg = (MessageFuncs *)(*global)("Info Messages", GFUSE_TRANSIENT);
	if (!msg)
		return AFUNC_BADGLOBAL;

	return AFUNC_OK;
}

/* ----------------------------------------------------------------
 * Server description
 * ---------------------------------------------------------------- */

ServerRecord ServerDesc[] = {
	{ "ShaderHandler",   "PBR",
	  (ActivateFunc *)Activate },
	{ "ShaderInterface", "PBR",
	  (ActivateFunc *)Interface },
	{ 0 }
};
