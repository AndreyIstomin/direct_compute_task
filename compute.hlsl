#define THREAD_X 32
#define THREAD_Y 32
#define SHARED_1

struct Particle
{
	float3 pos;
	float radius;
	float opacity;
};

StructuredBuffer<Particle> sbParticles;

RWBuffer<float> sbShadows;

//float3 sunDir; <-- check it
cbuffer globals
{
	float3 sunDir;
	float reserved;
};

#ifdef NAIVE

float Overlap(in Particle caster, in Particle receiver) {

	float sunDistDiff = dot(caster.pos - receiver.pos, sunDir);

	if (sunDistDiff < 0) {
		return 0.0f;
	}

	float3 dirReceiver = sunDir * dot(sunDir, receiver.pos);
	float3 dirCaster = sunDir * dot(sunDir, caster.pos);

	float3 posReceiver = receiver.pos - dirReceiver;
	float3 posCaster = caster.pos - dirCaster;

	float dist = length(posReceiver - posCaster);

	return caster.opacity * min(caster.radius * caster.radius / (receiver.radius * receiver.radius), 1.0f) * smoothstep(receiver.radius + caster.radius, abs(receiver.radius - caster.radius), dist);
}


[numthreads(THREAD_X, THREAD_Y, 1)]
void csComputeSelfShadowing(uint tid : SV_GroupIndex)
{
	float result = 1.0f;
	const int N = THREAD_X * THREAD_Y;

	for (int i = tid + 1; i < N + tid; ++i) {
		result *= 1.0f - Overlap(sbParticles[i % N], sbParticles[tid]);
	}

	sbShadows[tid] = result;
}

#elif defined SHARED

float Overlap(in Particle caster, in Particle receiver) {

	const float sunDistDiff = dot(caster.pos - receiver.pos, sunDir);
	const uint isCaster = sunDistDiff > 0;
	
	float dist = length(receiver.pos - caster.pos + sunDir * sunDistDiff);

	return isCaster  * caster.opacity * min(caster.radius * caster.radius / (receiver.radius * receiver.radius), 1.0f) * 
		smoothstep(receiver.radius + caster.radius, abs(receiver.radius - caster.radius), dist);
}

groupshared Particle smem[THREAD_X * THREAD_Y];

[numthreads(THREAD_X, THREAD_Y, 1)]
void csComputeSelfShadowing(uint tid : SV_GroupIndex)
{
	float result = 1.0f;
	const uint N = THREAD_X * THREAD_Y;

	const Particle p = sbParticles[tid];
	smem[tid] = p;

	GroupMemoryBarrierWithGroupSync();

	for (uint i = tid + 1; i < N + tid; ++i) {
		result *= 1.0f - Overlap(smem[i % N], p);
	}

	sbShadows[tid] = result;
}

#elif defined SHARED_1



float Overlap(in Particle caster, in Particle receiver) {

	const float sunDistDiff = caster.pos.x - receiver.pos.x;
	const uint isCaster = sunDistDiff > 0;

	const float dist = length(receiver.pos.yz - caster.pos.yz);

	return isCaster * caster.opacity * min(caster.radius * caster.radius / (receiver.radius * receiver.radius), 1.0f) * 
		smoothstep(receiver.radius + caster.radius, abs(receiver.radius - caster.radius), dist);
}

groupshared Particle smem[THREAD_X * THREAD_Y];

[numthreads(THREAD_X, THREAD_Y, 1)]
void csComputeSelfShadowing(uint tid : SV_GroupIndex)
{
	float result = 1.0f;
	const uint N = THREAD_X * THREAD_Y;
	const float3 sunZ = normalize(cross(sunDir, float3(0.0f, 1.0f, 0.0f))); // Допускаю, что sunDir не колинеарен Y
	const float3 sunY = cross(sunZ, sunDir);


	Particle p = sbParticles[tid];
	p.pos = float3(dot(p.pos, sunDir), dot(p.pos, sunY), dot(p.pos, sunZ));
	
	smem[tid] = p;

	GroupMemoryBarrierWithGroupSync();

	for (uint i = tid + 1; i < N + tid; ++i) {
		result *= 1.0f - Overlap(smem[i % N], p);
	}

	sbShadows[tid] = result;
}
#endif