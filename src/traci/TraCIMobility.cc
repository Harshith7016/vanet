//
// Copyright (C) 2006-2012 Christoph Sommer <christoph.sommer@uibk.ac.at>
//
// Documentation for these modules is at http://veins.car2x.org/
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//

#include <limits>
#include <iostream>
#include <sstream>

#include "veins/modules/mobility/traci/TraCIMobility.h"
//#include "vanet/src/traci/TraCIMobility.h"

using Veins::TraCIMobility;

Define_Module(Veins::TraCIMobility);

const simsignalwrap_t TraCIMobility::parkingStateChangedSignal = simsignalwrap_t(TRACI_SIGNAL_PARKING_CHANGE_NAME);

namespace {
	const double MY_INFINITY = (std::numeric_limits<double>::has_infinity ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::max());
}

void TraCIMobility::Statistics::initialize()
{
	firstRoadNumber = MY_INFINITY;
	startTime = simTime();
	totalTime = 0;
	stopTime = 0;
	minSpeed = MY_INFINITY;
	maxSpeed = -MY_INFINITY;
	totalDistance = 0;
	totalCO2Emission = 0;
}

void TraCIMobility::Statistics::watch(cSimpleModule& )
{
	WATCH(totalTime);
	WATCH(minSpeed);
	WATCH(maxSpeed);
	WATCH(totalDistance);
}

void TraCIMobility::Statistics::recordScalars(cSimpleModule& module)
{ }

void TraCIMobility::initialize(int stage)
{
	if (stage == 0)
	{
		BaseMobility::initialize(stage);

		debug = par("debug");
		antennaPositionOffset = par("antennaPositionOffset");
		accidentCount = par("accidentCount");

		currentPosXVec.setName("posx");
		currentPosYVec.setName("posy");
		currentSpeedVec.setName("speed");
		currentAccelerationVec.setName("acceleration");
		currentCO2EmissionVec.setName("co2emission");

		ASSERT(isPreInitialized);
		isPreInitialized = false;

		Coord nextPos = calculateAntennaPosition(roadPosition);
		nextPos.z = move.getCurrentPosition().z;

		move.setStart(nextPos);
		move.setDirectionByVector(Coord(cos(angle), -sin(angle)));
		move.setSpeed(speed);

		WATCH(road_id);
		WATCH(speed);
		WATCH(angle);

		isParking = false;

		startAccidentMsg = 0;
		stopAccidentMsg = 0;
		manager = 0;
		last_speed = -1;

		if (accidentCount > 0) {
			simtime_t accidentStart = par("accidentStart");
			startAccidentMsg = new cMessage("scheduledAccident");
			stopAccidentMsg = new cMessage("scheduledAccidentResolved");
			scheduleAt(simTime() + accidentStart, startAccidentMsg);
		}
	}
	else if (stage == 1)
	{
		// don't call BaseMobility::initialize(stage) -- our parent will take care to call changePosition later
	}
	else
	{
		BaseMobility::initialize(stage);
	}

}

void TraCIMobility::finish()
{
	cancelAndDelete(startAccidentMsg);
	cancelAndDelete(stopAccidentMsg);

	isPreInitialized = false;
}

void TraCIMobility::handleSelfMsg(cMessage *msg)
{
	if (msg == startAccidentMsg) {
		getVehicleCommandInterface()->setSpeed(0);
		simtime_t accidentDuration = par("accidentDuration");
		scheduleAt(simTime() + accidentDuration, stopAccidentMsg);
		accidentCount--;
	}
	else if (msg == stopAccidentMsg) {
		getVehicleCommandInterface()->setSpeed(-1);
		if (accidentCount > 0) {
			simtime_t accidentInterval = par("accidentInterval");
			scheduleAt(simTime() + accidentInterval, startAccidentMsg);
		}
	}
}

void TraCIMobility::preInitialize(std::string external_id, const Coord& position, std::string road_id, double speed, double angle)
{
	this->external_id = external_id;
	this->lastUpdate = 0;
	this->roadPosition = position;
	this->road_id = road_id;
	this->speed = speed;
	this->angle = angle;
	this->antennaPositionOffset = par("antennaPositionOffset");

	Coord nextPos = calculateAntennaPosition(roadPosition);
	nextPos.z = move.getCurrentPosition().z;

	move.setStart(nextPos);
	move.setDirectionByVector(Coord(cos(angle), -sin(angle)));
	move.setSpeed(speed);

	isPreInitialized = true;
}

void TraCIMobility::nextPosition(const Coord& position, std::string road_id, double speed, double angle, TraCIScenarioManager::VehicleSignal signals)
{
	if (debug) EV << "nextPosition " << position.x << " " << position.y << " " << road_id << " " << speed << " " << angle << std::endl;
	isPreInitialized = false;
	this->roadPosition = position;
	this->road_id = road_id;
	this->speed = speed;
	this->angle = angle;
	this->signals = signals;

	changePosition();
}

void TraCIMobility::changePosition()
{
	// ensure we're not called twice in one time step
	ASSERT(lastUpdate != simTime());

	// keep statistics (for current step)
	currentPosXVec.record(move.getStartPos().x);
	currentPosYVec.record(move.getStartPos().y);

	Coord nextPos = calculateAntennaPosition(roadPosition);
	nextPos.z = move.getCurrentPosition().z;

	this->lastUpdate = simTime();

	move.setStart(Coord(nextPos.x, nextPos.y, move.getCurrentPosition().z)); // keep z position
	move.setDirectionByVector(Coord(cos(angle), -sin(angle)));
	move.setSpeed(speed);
	if (hasGUI()) updateDisplayString();
	fixIfHostGetsOutside();
	updatePosition();
}

void TraCIMobility::changeParkingState(bool newState) {
	isParking = newState;
	emit(parkingStateChangedSignal, this);
}

void TraCIMobility::updateDisplayString() {
	ASSERT(-M_PI <= angle);
	ASSERT(angle < M_PI);

	getParentModule()->getDisplayString().setTagArg("b", 2, "rect");
	getParentModule()->getDisplayString().setTagArg("b", 3, "red");
	getParentModule()->getDisplayString().setTagArg("b", 4, "red");
	getParentModule()->getDisplayString().setTagArg("b", 5, "0");

	if (angle < -M_PI + 0.5 * M_PI_4 * 1) {
		getParentModule()->getDisplayString().setTagArg("t", 0, "\u2190");
		getParentModule()->getDisplayString().setTagArg("b", 0, "4");
		getParentModule()->getDisplayString().setTagArg("b", 1, "2");
	}
	else if (angle < -M_PI + 0.5 * M_PI_4 * 3) {
		getParentModule()->getDisplayString().setTagArg("t", 0, "\u2199");
		getParentModule()->getDisplayString().setTagArg("b", 0, "3");
		getParentModule()->getDisplayString().setTagArg("b", 1, "3");
	}
	else if (angle < -M_PI + 0.5 * M_PI_4 * 5) {
		getParentModule()->getDisplayString().setTagArg("t", 0, "\u2193");
		getParentModule()->getDisplayString().setTagArg("b", 0, "2");
		getParentModule()->getDisplayString().setTagArg("b", 1, "4");
	}
	else if (angle < -M_PI + 0.5 * M_PI_4 * 7) {
		getParentModule()->getDisplayString().setTagArg("t", 0, "\u2198");
		getParentModule()->getDisplayString().setTagArg("b", 0, "3");
		getParentModule()->getDisplayString().setTagArg("b", 1, "3");
	}
	else if (angle < -M_PI + 0.5 * M_PI_4 * 9) {
		getParentModule()->getDisplayString().setTagArg("t", 0, "\u2192");
		getParentModule()->getDisplayString().setTagArg("b", 0, "4");
		getParentModule()->getDisplayString().setTagArg("b", 1, "2");
	}
	else if (angle < -M_PI + 0.5 * M_PI_4 * 11) {
		getParentModule()->getDisplayString().setTagArg("t", 0, "\u2197");
		getParentModule()->getDisplayString().setTagArg("b", 0, "3");
		getParentModule()->getDisplayString().setTagArg("b", 1, "3");
	}
	else if (angle < -M_PI + 0.5 * M_PI_4 * 13) {
		getParentModule()->getDisplayString().setTagArg("t", 0, "\u2191");
		getParentModule()->getDisplayString().setTagArg("b", 0, "2");
		getParentModule()->getDisplayString().setTagArg("b", 1, "4");
	}
	else if (angle < -M_PI + 0.5 * M_PI_4 * 15) {
		getParentModule()->getDisplayString().setTagArg("t", 0, "\u2196");
		getParentModule()->getDisplayString().setTagArg("b", 0, "3");
		getParentModule()->getDisplayString().setTagArg("b", 1, "3");
	}
	else {
		getParentModule()->getDisplayString().setTagArg("t", 0, "\u2190");
		getParentModule()->getDisplayString().setTagArg("b", 0, "4");
		getParentModule()->getDisplayString().setTagArg("b", 1, "2");
	}
}

void TraCIMobility::fixIfHostGetsOutside()
{
	Coord pos = move.getStartPos();
	Coord dummy = Coord::ZERO;
	double dum;

	bool outsideX = (pos.x < 0) || (pos.x >= playgroundSizeX());
	bool outsideY = (pos.y < 0) || (pos.y >= playgroundSizeY());
	bool outsideZ = (!world->use2D()) && ((pos.z < 0) || (pos.z >= playgroundSizeZ()));
	if (outsideX || outsideY || outsideZ) {
		error("Tried moving host to (%f, %f) which is outside the playground", pos.x, pos.y);
	}

	handleIfOutside( RAISEERROR, pos, dummy, dummy, dum);
}

double TraCIMobility::calculateCO2emission(double v, double a) const {
return 0.0;
}

Coord TraCIMobility::calculateAntennaPosition(const Coord& vehiclePos) const {
	Coord corPos;
	if (antennaPositionOffset >= 0.001) {
		//calculate antenna position of vehicle according to antenna offset
		corPos = Coord(vehiclePos.x - antennaPositionOffset*cos(angle), vehiclePos.y + antennaPositionOffset*sin(angle), vehiclePos.z);
	} else {
		corPos = Coord(vehiclePos.x, vehiclePos.y, vehiclePos.z);
	}
	return corPos;
}
