#include "api.h"

#include "RTCube.cuh"

namespace CudaCore
{
	struct RTCubeP
	{
		::RTCube cube;
	};

	RTCube::RTCube(const IR::CubeDef&)
		: p(new RTCubeP())
	{}

	RTCube::~RTCube()
	{
		delete p;
	}

	void RTCube::insert(const IR::Rows&)
	{

	}

	IR::QueryResult RTCube::query(const IR::Query&)
	{
		return IR::QueryResult();
	}
}