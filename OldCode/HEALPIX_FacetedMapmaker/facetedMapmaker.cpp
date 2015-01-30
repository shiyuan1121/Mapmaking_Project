/* 
TODO:
* Convert to HEALPIX
* Make sure I figure out the signs on the k.b terms.

DONE:
* Create a "true" map by projecting the GSM onto a tangent plane at the facet location, so I can see how a convolution with the PSF compares to my dirty map.
* Test to see how the number of integrations in converges on the true map.
* Examine HERA, Omniscope, and MWA instantaneous beams at the zenith using Max's formula
* Make sure my explicitly calculated A^t (the matrix that converts from baseline space to image space) gives the same result in both parts of the code (the part that makes a map using an FFT and the part that computes the PSF).
	This works as long as I compare facets where the e^-k.b has been done on a per-basline basis and not on a gridded baseline basis...which gives the wrong answer to a few percent.
		This discrepancy doesn't matter in the long run, since the gridded phase-shifting cancels out when I calculate A^t N^-1 A
* It might be possible to make the code run faster by only computing Fourier modes where there are observations in any given facet.
	* Yes, I can save a bunch of time in the most time-intensive step by not summing over modes with Ninv[i][j] = 0;
* I don't need to know how pixels outside the facet are calculated...perhaps I should only look at how pixels inside the facet are affected by pixels outside the facet.
* Look again at whether we can easily fit polynomials to the PSF as a function of position
*/


#include <iostream>
#include <iomanip>
#include <fstream>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <vector>
#include <string>
#include <cstring>
#include <sstream>
#include "fftw3.h"
#include <ctype.h>
#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <vector>
#include <math.h>
#include <sstream>
#include <healpix_base.h>
#include <healpix_map.h>
#include <map>

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SETTINGS AND GLOBAL VARIABLES
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

string specsFilename = "Specifications.txt";

//Settings to load
double freq; //MHz
string polarization;
string baselinesFile, visibilitiesFolder, unnormalizedMapFilename, finalMapFilename, PSFfilename, healpixPixelFilename, extendedHealpixPixelFilename, noiseCovFilename, DmatrixFilename, dataProductFilePrefix, pixelCoordinatesFilename, extendedPixelCoordiantesFilename;
double angularResolution; //Degrees
double arrayLat; //Degrees
double arrayLong; //Degrees
double facetRA; //Degrees
double facetDec; //Degrees;
double integrationTime; //seconds
double channelBandwidth; //MHz
double snapshotTime; //seconds
int NSIDE; //Sets HEALPIX resolution
double mappingOverresolution; //sets FFT map overresolution
double mappingFieldSizeFactor; //effectively increases the resolution in gridded baseline space
double facetSize; //degrees
int PSFextensionBeyondFacetFactor; //Should be an odd integer. 1 is no padding.
double maximumAllowedAngleFromPBCenterToFacetCenter; //degrees
double xpolOrientationDegreesEastofNorth;
double noiseStd; //Jy on each visibilility...this number is totally made up
bool gaussianPrimaryBeam; //if true, use guassian PB at zenith, else use MWA dipoles (ala omniscope)
double primaryBeamFWHM; //FWHM in degrees if using a guassian PB
bool gridVisibilitiesWhenCalculatingPSF; //Should be false for highly redundant arrays and true for minimally redundant arrays

//Hard-coded settings and global variables
bool PSFpeaksAtOne = false; //Otherwise, the diagonal of the PSF matrix is set to be all ones
const double pi = 3.1415926535897932384626433832795;
const double c = 299792000; //m/s
int nAltBeamPoints = 45; //running from 0 to 90 in increments of 90/44
int nAzBeamPoints = 180; //running for 0 to 360 in increments of 360/179
double firstBeamFreq = 110; //MHz
double beamDeltaFreq = 10; //MHz
double lastBeamFreq = 190; //MHz
string beamDirectory = "../MWA_Primary_Beams/mwa_beam_";
int nFreqFiles = int((lastBeamFreq - firstBeamFreq)/10 + 1);
double deltaBeamAlt = 90.0 / (nAltBeamPoints-1);
double deltaBeamAz = 360.0 / (nAzBeamPoints-1);

//Global variables to compute later
int nIntegrationsToUse = 0;
int nPixels, nPixelsExtended, nBaselines, nLSTs, nSnapshots, nFull;
double k, facetDecInRad, facetRAinRad, angResInRad, latInRad, temperatureConversionFactor;


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// USEFUL DATA STRUCTURES
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//Stores 2D coordinates a and d
struct coord{
	int i, j;
	coord(int iIn, int jIn) : i(iIn), j(jIn) {}
};

//Stores complex numbers (e.g. visibilities). Can multiply them by real (double precision) or other complex numbers
struct complex{
	double re, im;
	complex(){}
	complex(double reIn, double imIn) : re(reIn), im(imIn) {}
	double abs(){ return sqrt(re*re + im*im); }
	complex conj(){ return complex(re, -im); }
	complex operator*(const complex& that){ return complex(re*that.re - im*that.im, re*that.im + im*that.re); }
	complex operator*(const double that){ return complex(re*that, im*that); }
	complex operator+(const complex& that){ return complex(re + that.re, im + that.im); }
	complex operator-(const complex& that){ return complex(re - that.re, im - that.im); }
	void print(){ cout << re << " + " << im << "i"; }
};


struct horizPoint; //Predeclaration
struct equaPoint; //Predeclaration

//When appropriate, +x is east, +y is north,  +z is up
struct cartVec{
	double x,y,z;	
	cartVec(){}
	cartVec(double xIn, double yIn, double zIn) : x(xIn), y(yIn), z(zIn) {}
	cartVec operator+(const cartVec& that){ return cartVec(x+that.x, y+that.y, z+that.z); }
	cartVec operator-(const cartVec& that){ return cartVec(x-that.x, y-that.y, z-that.z); }
	cartVec operator*(const double that){ return cartVec(x*that, y*that, z*that); }
	double dot(cartVec that){
		return (x*that.x + y*that.y + z*that.z);
	}
	cartVec cross(cartVec that){
		return cartVec(y*that.z - z*that.y, z*that.x - x*that.z, x*that.y - y*that.x);
	}
	cartVec normalize(){
		return (cartVec(x,y,z) * (1.0 / sqrt(x*x + y*y + z*z)));
	}
	horizPoint toHoriz();
	void print(){ cout << "["<< x << ", " << y << ", " << z << "]"; }
};

//Alt is 0 at the horizon, pi/2 at the zenith. Az is radians east of north
struct horizPoint{
	double alt, az;
	horizPoint(){}
	horizPoint(double altIn, double azIn) : alt(altIn), az(azIn) {}
	cartVec toVec(){
		cartVec cCoord(sin(az)*cos(alt), cos(az)*cos(alt), sin(alt));
		return cCoord;
	}
	double greatCircleAngle(horizPoint hp){
		horizPoint thisHP(alt, az);
		cartVec here = thisHP.toVec();
		cartVec there = hp.toVec();
		double dist = sqrt(pow(here.x - there.x,2) + pow(here.y - there.y,2) + pow(here.z - there.z,2));
		return 2*asin(dist/2);
	}
	equaPoint toEqua(double LST);
};

//Converts cartesian vectors to altitude and azimuth. I needed to delcate it outside the struct because horizPoint hadn't been declared yet.
horizPoint cartVec::toHoriz(){
	return horizPoint(asin(z/sqrt(x*x + y*y + z*z)), fmod(atan2(x,y)+2*pi,2*pi)); 
}

//Creates an equatorial pointing (RA and Dec) which can be converted into a horizontal pointing given an LST (assuming an array latitude as a global variable)
struct equaPoint{
	double ra, dec;
	equaPoint(){}
	equaPoint(double raIn, double decIn) : ra(raIn), dec(decIn) {}
	horizPoint toHoriz(double LST){
		double lha = pi/12.0*LST - ra; //radians
		horizPoint hp;
    	hp.alt = asin(sin(latInRad) * sin(dec) + cos(latInRad) * cos(dec) * cos(lha));
    	hp.az = atan2(sin(lha) * cos(dec), cos(lha) * cos(dec) * sin(latInRad) - sin(dec) * cos(latInRad)) + pi;
    	return hp;
	}
	pointing toHealpixPointing(){
		return pointing(pi/2-dec, ra); //pointing is defined as the colatitude (0 to pi = N to S) and the longitude (0 to 2pi)
	}
};

//Converts cartesian vectors to altitude and azimuth. I needed to delcate it outside the struct because horizPoint hadn't been declared yet.
equaPoint horizPoint::toEqua(double LST){
	equaPoint ep;
	ep.dec = asin(sin(alt)*sin(latInRad) - cos(alt)*cos(latInRad)*cos(az-pi));
	ep.ra = LST*2.0*pi/24 - atan2(sin(az-pi),(cos(az-pi)*sin(latInRad)+tan(alt)*cos(latInRad))); //The one I think is right
	return ep;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// LOADING AND INTIALIZATION FUNCTIONS
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//Loads specifications from Specifications.txt or a specified filename;
void loadSpecs(){
	fstream infile(specsFilename.c_str(),fstream::in);
	string dummy;
	infile >> dummy >> dummy >> freq;
	infile >> dummy >> dummy >> polarization;
	infile >> dummy >> dummy >> baselinesFile;
	infile >> dummy >> dummy >> visibilitiesFolder;
	infile >> dummy >> dummy >> unnormalizedMapFilename;
	infile >> dummy >> dummy >> finalMapFilename;
	infile >> dummy >> dummy >> PSFfilename;
	infile >> dummy >> dummy >> healpixPixelFilename;
	infile >> dummy >> dummy >> extendedHealpixPixelFilename;
	infile >> dummy >> dummy >> pixelCoordinatesFilename;
	infile >> dummy >> dummy >> extendedPixelCoordiantesFilename;
	infile >> dummy >> dummy >> noiseCovFilename;
	infile >> dummy >> dummy >> DmatrixFilename;
	infile >> dummy >> dummy >> dataProductFilePrefix;
	infile >> dummy >> dummy >> arrayLat;
	infile >> dummy >> dummy >> arrayLong;
	infile >> dummy >> dummy >> facetRA;
	infile >> dummy >> dummy >> facetDec;
	infile >> dummy >> dummy >> facetSize;
	infile >> dummy >> dummy >> NSIDE;
	infile >> dummy >> dummy >> mappingOverresolution;
	infile >> dummy >> dummy >> mappingFieldSizeFactor;
	infile >> dummy >> dummy >> PSFextensionBeyondFacetFactor;
	infile >> dummy >> dummy >> snapshotTime;
	infile >> dummy >> dummy >> integrationTime;
	infile >> dummy >> dummy >> channelBandwidth;
	infile >> dummy >> dummy >> gaussianPrimaryBeam;
	infile >> dummy >> dummy >> primaryBeamFWHM;
	infile >> dummy >> dummy >> xpolOrientationDegreesEastofNorth;
	infile >> dummy >> dummy >> maximumAllowedAngleFromPBCenterToFacetCenter;
	infile >> dummy >> dummy >> noiseStd;
	infile >> dummy >> dummy >> gridVisibilitiesWhenCalculatingPSF;
	infile.close();

	//Other global variables to set
	angularResolution = sqrt(4*pi / (12.0*NSIDE*NSIDE)) * 360/2/pi;
	nFull = int(round(mappingFieldSizeFactor * mappingOverresolution * 2 / (sqrt(4*pi / (12.0*NSIDE*NSIDE))) / 2)*2);
	k = 2 * pi * freq * 1e6 / c; 
	facetDecInRad = facetDec*pi/180;
	facetRAinRad = facetRA*pi/180;
	angResInRad = angularResolution*pi/180;
	latInRad = pi/180.0*arrayLat;
	temperatureConversionFactor = (c*c) / (2 * 1.3806488e-23 * pow(freq*1e6,2) * 1e26); //multiply by this number to convert Jy to K
	healpixPixelFilename = dataProductFilePrefix + healpixPixelFilename;
	extendedHealpixPixelFilename = dataProductFilePrefix + extendedHealpixPixelFilename;
	pixelCoordinatesFilename = dataProductFilePrefix + pixelCoordinatesFilename;
	extendedPixelCoordiantesFilename = dataProductFilePrefix + extendedPixelCoordiantesFilename;
	finalMapFilename = dataProductFilePrefix + finalMapFilename;
	PSFfilename = dataProductFilePrefix + PSFfilename;
	noiseCovFilename = dataProductFilePrefix + noiseCovFilename;
	DmatrixFilename = dataProductFilePrefix + DmatrixFilename;
}

//Loads baselines (only one orientation, no complex conjugates, from the file under the global variable "baselinesFile", which is in "south east up" format)
vector<cartVec> loadBaselines(vector<int>& baselineRedundancy){
	cout << "Now loading all the baseline vectors." << endl << "[NOTE: THIS ASSUMES THAT THE DATA FORMAT IS SOUTH EAST UP, THOUGH THE INTERNAL FORMAT IS EAST NORTH UP (RIGHT HANDED SYSTEM)]" << endl;
	vector<cartVec> baselines;
	double south, east, up;
	int multiplicity;
	fstream infile(baselinesFile.c_str(),fstream::in);
	while(infile >> south >> east >> up >> multiplicity){
		cartVec thisBaseline;
		thisBaseline.y = -south; 
		thisBaseline.x = east;
		thisBaseline.z = up;
		baselines.push_back(thisBaseline);
		baselineRedundancy.push_back(multiplicity);
	}
	infile.close();
	nBaselines = baselines.size();
	return baselines;
}

//Loads the pre-computed principal axes of the plane of the array, which is, in general, slightly sloped relative to the the EW/NS plane, and a third axis perpendicular to the two and pointed mostly up
//The two axes must be perpendicular to one another. By convention, the first vector will be mostly East with no N/S component and the second will be mostly South
//The rotation can be paraterized by two cartesian rotations, one about the x (EW) axis and one about the y axis (SN)...this is completely general.
//Since rotations are not commutative, I'll first perform the rotation around the x axis and then the rotation about the y axis. This will preserve the property that x has no N/S component.
vector<cartVec> loadArrayPrincipalAxes(){
	cout << "Now loading array's principal axes...[NOTE: THESE ARE HARD CODED TO AN ARBITARY (SMALL) VALUE OR 0 FOR NOW.]" << endl;
	double thetaX = 0;//.02; //radians of rotation about the EW axis taking north toward up.
	double thetaY = 0;//.04; //radians of rotation about the NS axis taking east toward up.
	/*cartVec axisE(1, 0, 0); //Due east
	cartVec axisN(0, 1, 0); //Due south
	cartVec axisErotX(1, 0, 0);
	cartVec axisNrotX(0, cos(thetaX), sin(thetaX));*/
	cartVec axisErotXY(cos(thetaY), 0, sin(thetaY));
	cartVec axisNrotXY(-sin(thetaX)*sin(thetaY), cos(thetaX), sin(thetaX)*cos(thetaY)); 
	cartVec axisUrotXY = axisErotXY.cross(axisNrotXY);
	vector<cartVec> arrayPrincipalAxes;
	arrayPrincipalAxes.push_back(axisErotXY);		
	arrayPrincipalAxes.push_back(axisNrotXY);
	arrayPrincipalAxes.push_back(axisUrotXY);
	return arrayPrincipalAxes;
}

//This figures out the projection of the baselines into the best-fit plane of the array. This is needed for the 2D FFT.
//The projected baselines are written in terms of their components in terms of the array principal axes, not in terms of East, North, and Up.
vector<cartVec> calculateProjectedBaselines(vector<cartVec>& baselines, vector<cartVec>& arrayPrincipalAxes){
	vector<cartVec> projectedBaselines;
	for (int b = 0; b < nBaselines; b++){
		cartVec	projectedBaselineInENU = baselines[b] - arrayPrincipalAxes[2]*(baselines[b].dot(arrayPrincipalAxes[2]));
		cartVec projectedBaselineInXYZ(projectedBaselineInENU.dot(arrayPrincipalAxes[0]), projectedBaselineInENU.dot(arrayPrincipalAxes[1]), 0.0);
		projectedBaselines.push_back(projectedBaselineInXYZ);
	} 
	return	projectedBaselines;
}

//Loads visibilities, which have an LST and a real and imaginary component, from a file with the format I used for the omniscope
//Format is allVisibilites[baseline number same as baselines vector][different measurements].re/im
//It is assumed that all visibilities have the same set of LSTs, which is taken from the first baseline loaded
vector< vector<complex> > loadVisibilities(vector<cartVec>& baselines, vector<double>& LSTs){	
	vector< vector<complex> > allVisibilities;
	cout << "Now loading all visibilities for " << freq << " MHz and " << polarization << " polarization..." << endl;
	for (int n = 0; n < nBaselines; n++){
		cout << " " << floor(100.0 * n / nBaselines) << "% done. \r" << std::flush;

		//Format filename
		stringstream ss;
		ss << visibilitiesFolder  << -baselines[n].y << "_m_south_" << baselines[n].x << "_m_east_" << baselines[n].z << "_m_up_" << polarization << "_pol_" << freq << "_MHz.dat";
		//ss << visibilitiesFolder << "Visibilties_for_" << round(-baselines[n].y) << "_m_south_" << round(baselines[n].x) << "_m_east_" << round(baselines[n].z) << "_m_up_" << polarization << "_pol_" << freq << "_MHz.dat";
		string infilename = ss.str();
		fstream infile(infilename.c_str(),fstream::in);
		
		//Load all LST, re, and im into a vector of vectors of visibilities
		vector<complex> thisBaselinesVisibilities;
		double localSiderealTime, real, imaginary;
		while(infile >> localSiderealTime >> real >> imaginary){
			if (n == 0) LSTs.push_back(localSiderealTime);
			complex thisVisibility(real, imaginary);
			thisBaselinesVisibilities.push_back(thisVisibility);
		}
		infile.close();
		if (thisBaselinesVisibilities.size() == 0) cout << "WARNING: Cannot find " << infilename << endl;
		allVisibilities.push_back(thisBaselinesVisibilities);
	}
	nLSTs = LSTs.size();
	cout << "Done.                  " << endl;  
	return allVisibilities;
}

vector< vector<double> > loadNoiseVarianceOnEachVisibiltiy(vector<cartVec>& baselines, vector<double>& LSTs, vector<int> baselineRedundancy){
	//TODO: ENVENTUALLY I'LL WANT TO LOAD A MODEL FOR THE NOISE ON ANTENNA AND THEN COMPUTE WHAT THE PER VISIBILITY NOISE IS. FOR NOW I'LL JUST USE A SIMPLE MODEL.
	cout << "Now loading and computing the noise variance on each visibility...[NOTE: NOISE SET TO A SIMPLISTIC MODEL FOR NOW.]" << endl;
	vector< vector<double> > noiseVarianceOnEachVisibility(nBaselines, vector<double>(nLSTs,0));
	cout << endl << "ALL NOISE VARIANCES SET TO 1" << endl << endl;
	for (int t = 0; t < nLSTs; t++){
		for (int b = 0; b < nBaselines; b++){
			//noiseVarianceOnEachVisibility[b][t] = pow(noiseStd,2)/(integrationTime * channelBandwidth * 1e6 * baselineRedundancy[b]);
			noiseVarianceOnEachVisibility[b][t] = 1;
		}
	}
	return noiseVarianceOnEachVisibility;
}

//This function loads the location of the primary beam in horizontal coordinates as a function of LST
vector<horizPoint> loadPBPointings(vector<double>& LSTs){
	//TODO: EVENTUALLY, WE WANT TO GENERALIZE THIS TO WHERE THE PRIMARY BEAM IS POINTED, BUT FOR NOW WE'LL ASSUME IT'S THE ZENITH
	cout << "Now loading all the primary beam pointings..." << endl;
	vector<horizPoint> PBpointings;
	for (int n = 0; n < nLSTs; n++){
		horizPoint thisPointing(pi/2, 0);
		PBpointings.push_back(thisPointing);
	}
	return PBpointings;
}

//This funciton loads the primary beam into a array of discretized values of alt and az.
//The data file, somewhat inconsistently, has azimuth 0 in the direction pointed by the XX polarization and increases CCW
vector< vector<double> > loadDiscretizedPrimaryBeam(){
	cout << "Now loading the primary beams for the nearest two frequencies and interpolating/extrapolating between them..." << endl;
	cout << "[NOTE: FOR NOW, PRIMARY BEAM IS ASSUMED TO BE THE SAME FOR ALL OBSERVATIONS]" << endl;
	vector< vector<double> > primaryBeamDiscretized(nAltBeamPoints, vector<double>(nAzBeamPoints, 0.0));
	double freq1 = -1;
	double freq2 = -1;
	for (double f = firstBeamFreq; f <= lastBeamFreq; f+=beamDeltaFreq){
		if (fabs(f - freq) < fabs(freq1 - freq)) freq1 = f;	
	} 
	for (double f = firstBeamFreq; f <= lastBeamFreq; f+=beamDeltaFreq){
		if ((fabs(f - freq) < fabs(freq2 - freq)) && (f != freq1)) freq2 = f;
	}
	stringstream freq1stream, freq2stream;
	freq1stream << beamDirectory << polarization << "_" << freq1 << ".dat";
	freq2stream << beamDirectory << polarization << "_" << freq2 << ".dat";
	string file1 = freq1stream.str();
	string file2 = freq2stream.str();
	fstream infile1(file1.c_str(),fstream::in);
	fstream infile2(file2.c_str(),fstream::in);
	double gain1, gain2;
	for (int alt = 0; alt < nAltBeamPoints; alt++){
		for (int az = 0; az < nAzBeamPoints; az++){
			infile1 >> gain1;
			infile2 >> gain2;
			primaryBeamDiscretized[alt][az] = (gain2 - gain1)/(freq2 - freq1) * (freq - freq1) + gain1;
		}
	}
	return primaryBeamDiscretized;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// GEOMETRY FUNCTIONS
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//This function computes the altitude and azimuth of the facet ceneter for all LSTs observed.
vector<horizPoint> computeFacetCenterPointings(vector<double>& LSTs){
	cout << "Now computing the horizontal angle to the facet center for all LSTs..." << endl;
	vector<horizPoint> facetCenterPointings;
	equaPoint facetCenter(facetRAinRad, facetDecInRad);
	for (int n = 0; n < nLSTs; n++) facetCenterPointings.push_back(facetCenter.toHoriz(LSTs[n]));
	return facetCenterPointings;
}

//This function determines whether the distance between the facet center and the center of the maximumAllowedAngleFromPBCenterToFacetCenter
vector<bool> determineIntegrationsToUse(vector<horizPoint>& PBpointings, vector<horizPoint>& facetCenterPointings){
	vector<bool> toUse;
	for (int n = 0; n < PBpointings.size(); n++){ 
		toUse.push_back(facetCenterPointings[n].greatCircleAngle(PBpointings[n]) < pi/180*maximumAllowedAngleFromPBCenterToFacetCenter);
		if (toUse[n]) nIntegrationsToUse++;
	}
	cout << nIntegrationsToUse << " of the " << PBpointings.size() << " total integrations have the facet center within " << maximumAllowedAngleFromPBCenterToFacetCenter << " degrees of the primary beam center." << endl;
	return toUse;
}

//This functiona assigns LST indices to snapshot indices, which are gridded together
vector< vector<int> > assignLSTsToSnapshots(vector<bool> integrationsToUse){
	vector< vector<int> > snapshotLSTindices;
	for (int t = 0; t < nLSTs; t++){		
		if (integrationsToUse[t]){ //if ready to start a snapshot
			vector<int> integrationIndices;
			for (int t2 = 0; (t2 < snapshotTime/integrationTime && t < nLSTs); t2++){ //loop over a whole snapshot or until the end of observations
				if (integrationsToUse[t]) integrationIndices.push_back(t); //accumulate good integrations
				t++; //advance integration counter
			}
			t--; //prevents skipping
			snapshotLSTindices.push_back(integrationIndices); //accumulate snapshots
		}
	}
	nSnapshots = snapshotLSTindices.size();
	cout << "These integrations have been grouped into " << nSnapshots << " snapshots of at most " << snapshotTime << " seconds." << endl;
	return snapshotLSTindices;
}

//This function creates an empty healpix_map of doubles 
Healpix_Map<double> emptyHealpixMap(){
	Healpix_Map<double> emptyMap(int(round(log(NSIDE)/log(2))), RING);
	for (int n = 0; n < (12*NSIDE*NSIDE); n++) emptyMap[n] = 0;
	return emptyMap;
}

//This function figures out which HEALPIX indices are part of map and which are part of the extended map (used for computing the PSF)
vector<int> computeHealpixIndices(bool extended){
	Healpix_Map<double> sampleHealpixMap = emptyHealpixMap();
	equaPoint facetCenterEquaPoint(facetRAinRad, facetDecInRad);
	vector<int> healpixIndices;
	double angularRadius = facetSize/360*2*pi/2.0;
	if (extended) angularRadius *= PSFextensionBeyondFacetFactor;
	sampleHealpixMap.query_disc(facetCenterEquaPoint.toHealpixPointing(), angularRadius, healpixIndices);
	if (extended){
		nPixelsExtended = healpixIndices.size();
	} else {
		nPixels = healpixIndices.size();
	}
	return healpixIndices;
}

//This function creates a vector of pointings in equatorial coordinates for each pixel in the HEALPIX map
vector<equaPoint> computeEquaPointings(vector<int>& indices){
	Healpix_Map<double> sampleHealpixMap = emptyHealpixMap();
	vector<equaPoint> pixelEquaPointings;
	for (int n = 0; n < indices.size(); n++){
		pointing healpixPointing = sampleHealpixMap.pix2ang(indices[n]);
		equaPoint thisEquaPoint(healpixPointing.phi, pi/2 - healpixPointing.theta);
		pixelEquaPointings.push_back(thisEquaPoint);
	}
	return pixelEquaPointings;
}

//Creates a healpix map of bools for whether or not a given healpix pixel is in the part of the sky we're trying to map.
Healpix_Map<bool> isInFacet(vector<int>& healpixIndices){
	int order = int(round(log(NSIDE)/log(2)));
	Healpix_Map<bool> mapOfPixelsInFacet(order, RING);
	for (int n = 0; n < NSIDE*NSIDE*12; n++) mapOfPixelsInFacet[n] = false;
	for (int i = 0; i < healpixIndices.size(); i++) mapOfPixelsInFacet[healpixIndices[i]] = true;
	return mapOfPixelsInFacet;
}

//Creates a vector that maps where map[nth index in healpixIndices] = mth index in extendedHealpixIndices
vector<int> mapIndicesFromFacetToExtendedFacet(vector<int>& healpixIndices, vector<int>& extendedHealpixIndices){
	vector<int> mapOfIndicesInExtendedIndexVector(nPixels, 0);
	int extendedCounter = 0;
	for (int n = 0; n < nPixels; n++){
		while (extendedHealpixIndices[extendedCounter] != healpixIndices[n]){
			extendedCounter++;
			if (extendedCounter >= nPixelsExtended){
				cout << "UNABLE TO MATCH EXTENDED FACET PIXEL INDICES TO FACET PIXEL INDICES!!!" << endl;
			}
		}
		mapOfIndicesInExtendedIndexVector[n] = extendedCounter;
	}
	return mapOfIndicesInExtendedIndexVector;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// REPHASE AND RENORMALIZE
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//This function calculates N^-1 y, the first step in the mapmaking algorithm
void noiseInverseVarianceWeight(vector< vector<complex> >& allVisibilities, vector< vector<double> >& noiseVarianceOnEachVisibiltiy, vector<bool>& integrationsToUse){
	cout << "Now multiplying all visibilities by the inverse noise variance...[NOTE: FOR NOW, EACH VISIBILITY HAS A NOISE VARIANCE OF 1]" << endl;
	for (int t = 0; t < nLSTs; t++){
		if (integrationsToUse[t]){
			for (int b = 0; b < nBaselines; b++) allVisibilities[b][t] = allVisibilities[b][t]*(1.0/noiseVarianceOnEachVisibiltiy[b][t]);
		}
	}
}

//This function multiplies each visibility by e^(i*b.k_0) where b is the baseline vector and k_0 points to the facet center
void rephaseVisibilities(vector< vector<complex> >& allVisibilities, vector<bool>& integrationsToUse, vector<cartVec>& baselines, vector<horizPoint>& facetCenterPointings){
	cout << "Now rephasing all visibilities to the facet center..." << endl;
	for (int t = 0; t < nLSTs; t++){
		if (integrationsToUse[t]){
			for (int b = 0; b < nBaselines; b++){
				double b_dot_k = baselines[b].dot(facetCenterPointings[t].toVec())*(k); 
				complex complexFactor = complex(cos(b_dot_k), sin(b_dot_k)); //In A we multiply by e^b.k so in A^t we multiply by e^-b.k
				allVisibilities[b][t] = allVisibilities[b][t]*(complexFactor);
			}
		}
	}
}

void convertToTemperatureUnits(vector< vector<complex> >& allVisibilities, vector<bool>& integrationsToUse){
	cout << "Now converting the visibilities to temperature units..." << endl;
	for (int t = 0; t < nLSTs; t++){
		if (integrationsToUse[t]){
			for (int b = 0; b < nBaselines; b++) allVisibilities[b][t] = allVisibilities[b][t] * temperatureConversionFactor;
		}
	}
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// GRIDDING VISIBILITIES TOGETHER AND FFT
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


//This function converts baselines to array indices accoring to the formula j = n * Delta Theta * k * b_alpha/ (2*pi) and then grids up the visibilities and their complex conjugates
//During a given snapshot, baselines do not move.
void addVisibilitiesToGridInUV(vector< vector<complex> >& uvGrid, vector<cartVec>& projectedBaselines, vector<cartVec>& arrayPrincipalAxes, 
	vector<horizPoint>& facetCenterPointings, int LSTindex, double LST, vector< vector<complex> >& allVisibilities){
	
	double deltak =  k * sqrt(4*pi / (12.0*NSIDE*NSIDE)) / mappingOverresolution;
	for (int b = 0; b < nBaselines; b++){
		int xIndex = int(round(deltak * projectedBaselines[b].x * nFull / (2*pi)));
		int yIndex = int(round(deltak * projectedBaselines[b].y * nFull / (2*pi)));
		if ((abs(xIndex) < nFull/2) && (abs(yIndex) < nFull/2)){
			uvGrid[nFull/2 + xIndex][nFull/2 + yIndex] = uvGrid[nFull/2 + xIndex][nFull/2 + yIndex] + allVisibilities[b][LSTindex];
			uvGrid[nFull/2 - xIndex][nFull/2 - yIndex] = uvGrid[nFull/2 - xIndex][nFull/2 - yIndex] + allVisibilities[b][LSTindex].conj();
		}
	}

	int counter = 0;
	for (int i = 0; i < nFull; i++){
		for (int j = 0; j < nFull; j++){
			if (uvGrid[i][j].re != 0 && uvGrid[i][j].im != 0){
				double bx = (i - nFull/2) * 2 * pi / nFull / deltak;
				double by = (j - nFull/2) * 2 * pi / nFull / deltak;
				cout << setprecision(10) << counter << " is at (" << bx << ", " << by << ") with N^-1*y = " << uvGrid[i][j].re << " + " << uvGrid[i][j].im << "i." << endl;
				counter++;
				if (counter > 4) break;


			}
			
			
		}
		if (counter > 4) break;
	}
}

//This function takes the accumulated, gridded visibilities for a given snapshot and performs an FFT
vector< vector<double> > performUnnormalizedShiftedFFT(vector< vector<complex> >& uvGrid, int nAlpha, int nDelta, bool forward){
	fftw_complex *in, *out;
	in = (fftw_complex*) fftw_malloc(nAlpha*nDelta * sizeof(fftw_complex));
	out = (fftw_complex*) fftw_malloc(nAlpha*nDelta * sizeof(fftw_complex));
	//Shift and populate FFTW object
	for (int a = 0; a < nAlpha; a++){
		for (int d = 0; d < nDelta; d++){
			int shiftedAlpha = (a + nAlpha/2)%nAlpha;
			int shiftedDelta = (d + nDelta/2)%nDelta;
			in[shiftedAlpha*nDelta + shiftedDelta][0] = uvGrid[a][d].re;
			in[shiftedAlpha*nDelta + shiftedDelta][1] = uvGrid[a][d].im;
		}
	}

	//Perform FFT
	fftw_plan FT;
	if (forward){ 
		FT = fftw_plan_dft_2d(nAlpha, nDelta, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
	} else {
		FT = fftw_plan_dft_2d(nAlpha, nDelta, in, out, FFTW_BACKWARD, FFTW_ESTIMATE);
	}
	fftw_execute(FT);

	//Shift and put back into facetMap
	vector< vector<double> > facetMap(nAlpha, vector<double>(nDelta, 0.0));
	for (int a = 0; a < nAlpha; a++){
		for (int d = 0; d < nDelta; d++){
			int shiftedAlpha = (a + nAlpha/2)%nAlpha;
			int shiftedDelta = (d + nDelta/2)%nDelta;
			facetMap[a][d] = out[shiftedAlpha*nDelta + shiftedDelta][0];
		}
	}

	fftw_destroy_plan(FT);
	fftw_free(in); fftw_free(out);
	return facetMap;
}

//This function retruns the value of the gain of the PB as a funciton of altitude and azimuth
//Unfortunately, the primary beam azimuth is stored with the XX polarization as azimuth zero and continues CCW. 
double primaryBeam(horizPoint& pointing, vector< vector<double> >& PB){
	return 1;
	if (gaussianPrimaryBeam){
		if (pointing.alt > 0){
			double sigma = primaryBeamFWHM/360.0*2*pi/2.355;
			return exp(-pow(pi/2 -pointing.alt,2)/2/pow(sigma,2))/sigma/sqrt(2*pi);
		} else {
			return 0.0;
		}
	}
	if (pointing.alt <= 0) return 0.0;

	double altPixel = pointing.alt * 180.0 / pi / deltaBeamAlt;
	double azPixel = fmod(-pointing.az * 180.0 / pi + xpolOrientationDegreesEastofNorth + 360.0,360.0) / deltaBeamAz;
	int altIndex1 = int(floor(altPixel));
	int altIndex2 = int(ceil(altPixel));
	int azIndex1 = int(floor(azPixel));
	int azIndex2 = int(ceil(azPixel));
	
	//Handle integer pixel values to avoid getting 0/0
	if ((altIndex1 == altIndex2) && (azIndex1 == azIndex2)) return (PB[altIndex1][azIndex1]);
	if (altIndex1 == altIndex2) return ((PB[altIndex1][azIndex2] - PB[altIndex1][azIndex1]) * (azPixel - azIndex1) + PB[altIndex1][azIndex1]);
	if (azIndex1 == azIndex2) return ((PB[altIndex2][azIndex1] - PB[altIndex1][azIndex1]) * (altPixel - altIndex1) + PB[altIndex1][azIndex1]);

	double PBresult = (PB[altIndex1][azIndex1] * (altIndex2-altPixel) * (azIndex2-azPixel));
	PBresult += (PB[altIndex2][azIndex1] * (altPixel-altIndex1) * (azIndex2-azPixel));
	PBresult += (PB[altIndex1][azIndex2] * (altIndex2-altPixel) * (azPixel-azIndex1));
	PBresult += (PB[altIndex2][azIndex2] * (altPixel-altIndex1) * (azPixel-azIndex1));
	return PBresult;
}

//This function takes the 2D map projected onto the plane of the array and puts it back on the correct healpix pixels. 
//This function also deals with the beam and geometric factors that go into A^t
//TODO: figure out whether it should be a straight average or possibly some kind of beam-weighted average 

void addSnapshotMapToHealpixWithBeamAndGeometricFactors(vector< vector<double> >& snapshotMap, Healpix_Map<double>& coaddedMap, Healpix_Map<bool>& mapOfPixelsInFacet, 
	vector<int>& healpixIndices, vector<cartVec>& arrayPrincipalAxes, vector< vector<double> >& PB, horizPoint facetCenter, double snapshotCentralLST){

	double deltak =  k * sqrt(4*pi / (12.0*NSIDE*NSIDE)) / mappingOverresolution;
	Healpix_Map<double> thisSnapshot = emptyHealpixMap();
	Healpix_Map<double> pixelAreaAccumulated = emptyHealpixMap();
	cartVec facetCenterVec = facetCenter.toVec();
	
	double kxOffset = k*facetCenterVec.dot(arrayPrincipalAxes[0]);
	double kyOffset = k*facetCenterVec.dot(arrayPrincipalAxes[1]);

	for (int i = 0; i < nFull; i++){
		for (int j = 0; j < nFull; j++){
			double kx = (i - nFull/2.0) * deltak + kxOffset;
			double ky = (j - nFull/2.0) * deltak + kyOffset;
			if ((kx*kx + ky*ky) < (k*k)){ //otherwise it projects to outside the celestial sphere
				double kz = sqrt(k*k - kx*kx - ky*ky);
				cartVec thisPixelVector = arrayPrincipalAxes[0] * (kx/k) + arrayPrincipalAxes[1] * (ky/k) + arrayPrincipalAxes[2] * (kz/k);
				horizPoint thisHorizPoint = thisPixelVector.toHoriz();
				equaPoint thisEquaPoint = thisHorizPoint.toEqua(snapshotCentralLST);
				int thisHealpixPixel = coaddedMap.ang2pix(thisEquaPoint.toHealpixPointing());
				if (mapOfPixelsInFacet[thisHealpixPixel]){ //these are pixels in the facet
					double PBfactor = primaryBeam(thisHorizPoint, PB);
					double geometryFactor = pow(deltak,2) / (k * fabs(kz));
					thisSnapshot[thisHealpixPixel] += (snapshotMap[i][j] * PBfactor * geometryFactor);
					pixelAreaAccumulated[thisHealpixPixel] += geometryFactor;
				}
			} 
		}
		//cout << endl;
	}


	cout << "Not correcting mapmaking for pixel-size errors!!!" << endl << endl;
	for (int n = 0; n < nPixels; n++){
		//coaddedMap[healpixIndices[n]] += thisSnapshot[healpixIndices[n]]/ pixelAreaAccumulated[healpixIndices[n]] * (4*pi / (12.0*NSIDE*NSIDE));
		coaddedMap[healpixIndices[n]] += thisSnapshot[healpixIndices[n]];
	} 
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// POINT SPREAD FUNCTION CALCULATION
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//This function calculates the inverse noise variance for the gridded visibilities OR on the baselines themselves during the given snapshot
//Since variances add for the sum of uncorrelated random variables, I'm simply computing the inverse variance by adding together the variances on each visibility by assigning them to cells.
//TODO: I could use the symmetry to reduce the multiplications in this space by a factor of 2
vector<double> calculateNinv(vector<cartVec>& sampledBaselines, vector<cartVec>& correspondingPhysicalBaselines, vector<cartVec>& projectedBaselines, vector< vector<double> >& noiseVarianceOnEachVisibiltiy, vector<cartVec>& arrayPrincipalAxes, vector<int>& LSTindices, vector<double>& LSTs){
	vector< vector<double> > griddedNinv(nFull, vector<double>(nFull, 0.0));
	vector< vector<int> > keepTrackOfWhereBaselinesEndUp(nFull, vector<int>(nFull,- 1));
	vector<double> Ninv;
	vector<int> thisSampledBaselineMapsToThatPhysicalBaseline;
	if (gridVisibilitiesWhenCalculatingPSF){ //This case is for non-redundant arrays where it's best to grid together similar baselines to save computational cost
		double deltak =  k * sqrt(4*pi / (12.0*NSIDE*NSIDE)) / mappingOverresolution;
		for (int i = 0; i < LSTindices.size(); i++){
			for (int b = 0; b < nBaselines; b++){
				int xIndex = int(round(deltak * projectedBaselines[b].x * nFull / (2*pi)));
				int yIndex = int(round(deltak * projectedBaselines[b].y * nFull / (2*pi)));
				if ((abs(xIndex) < nFull/2) && (abs(yIndex) < nFull/2)){
					griddedNinv[nFull/2 + xIndex][nFull/2 + yIndex] = griddedNinv[nFull/2 + xIndex][nFull/2 + yIndex] + 1.0/noiseVarianceOnEachVisibiltiy[b][LSTindices[i]];
					griddedNinv[nFull/2 - xIndex][nFull/2 - yIndex] = griddedNinv[nFull/2 - xIndex][nFull/2 - yIndex] + 1.0/noiseVarianceOnEachVisibiltiy[b][LSTindices[i]];
					if (keepTrackOfWhereBaselinesEndUp[nFull/2 + xIndex][nFull/2 + yIndex] == -1) keepTrackOfWhereBaselinesEndUp[nFull/2 + xIndex][nFull/2 + yIndex] = b;
					if (keepTrackOfWhereBaselinesEndUp[nFull/2 - xIndex][nFull/2 - yIndex] == -1) keepTrackOfWhereBaselinesEndUp[nFull/2 - xIndex][nFull/2 - yIndex] = b+nBaselines;
				}
			}
		}
		for (int i = 0; i < nFull; i++){
			for (int j = 0; j < nFull; j++){
				if (griddedNinv[i][j] > 0){
					cartVec thisGriddedBaseline = arrayPrincipalAxes[0]*((i - nFull/2) * (2*pi) / (deltak * nFull)) + arrayPrincipalAxes[1]*((j - nFull/2) * (2*pi) / (deltak * nFull));
					sampledBaselines.push_back(thisGriddedBaseline);
					Ninv.push_back(griddedNinv[i][j]);
					thisSampledBaselineMapsToThatPhysicalBaseline.push_back(keepTrackOfWhereBaselinesEndUp[i][j]);
				}
			}
		}



		for (int b = 0; b < sampledBaselines.size(); b++){
			//cout << "b = " << b << endl;
			int physicalBaselineIndex = thisSampledBaselineMapsToThatPhysicalBaseline[b] % nBaselines;
			//cout << "sampled: "; sampledBaselines[b].print(); cout << endl;
			//cout << "physical: ";
			if (thisSampledBaselineMapsToThatPhysicalBaseline[b] < nBaselines){
				//projectedBaselines[physicalBaselineIndex].print();
				correspondingPhysicalBaselines.push_back(projectedBaselines[physicalBaselineIndex]);
			} else {
				//(projectedBaselines[physicalBaselineIndex] * -1.0).print();
				correspondingPhysicalBaselines.push_back(projectedBaselines[physicalBaselineIndex] * -1.0);
			}
			//cout << endl << endl;
		}
	


	} else { //This case is for highly redundant arrays like Omniscope or HERA
		for (int b = 0; b < nBaselines; b++){
			double noiseHere = 0;
			for (int i = 0; i < LSTindices.size(); i++) noiseHere += 1.0/noiseVarianceOnEachVisibiltiy[b][LSTindices[i]];
			sampledBaselines.push_back(projectedBaselines[b]);
			Ninv.push_back(noiseHere);
			sampledBaselines.push_back(projectedBaselines[b] * (-1.0));
			Ninv.push_back(noiseHere);
		}
	}
	return Ninv;
}

//This function looks at Ninv to figure out which values of i and j correspond to baselines that have been sampled. Then the baseline-facing part of A^t can be treated as a sparse matrix, saving memory
/*vector<coord> listAllSampledBaselines(vector< vector<double> >& Ninv){
	vector<coord> sampledBaselines;
	for (int i = 0; i < nFull; i++){
		for (int j = 0; j < nFull; j++){
			if (Ninv[i][j] > 0){
				coord sampled(i,j);
				sampledBaselines.push_back(sampled);
			}
		}
	}
	return sampledBaselines;
}*/

//for testing
vector< vector<complex> > calculateKAtransposeVersion2(double LST, vector<int>& extendedHealpixIndices, vector<equaPoint>& extendedPixelEquaPointings, vector<cartVec>& arrayPrincipalAxes, 
	vector<cartVec>& projectedBaselines, vector< vector<double> >& PB, horizPoint facetCenter, vector<cartVec>& sampledBaselines, vector<cartVec>& correspondingPhysicalBaselines, Healpix_Map<bool>& mapOfPixelsInExtendedFacet, Healpix_Map<bool>& mapOfPixelsInFacet){

	int nSampled = sampledBaselines.size();
	vector< vector<complex> > Atranspose(nPixelsExtended, vector<complex>(nSampled, complex(0,0)));
	double kxOffset = k*((facetCenter.toVec()).dot(arrayPrincipalAxes[0]));
	double kyOffset = k*((facetCenter.toVec()).dot(arrayPrincipalAxes[1]));

	cout << "kxOffset: " << kxOffset << "   kyOffset: " << kxOffset << endl; 
	double deltak =  k * sqrt(4*pi / (12.0*NSIDE*NSIDE)) / mappingOverresolution;
	Healpix_Map<double> emptyMap = emptyHealpixMap();
	vector< vector<int> > thisPixelsFacetCoordinate(nFull, vector<int>(nFull, -1));

	map<int,int> extendedFacetPixelMapping;
	for (int n = 0; n < nPixelsExtended; n++) extendedFacetPixelMapping[extendedHealpixIndices[n]] = n;


	vector<complex> baselineSpaceFactor(nSampled, complex(0,0));
	for (int b = 0; b < nSampled; b++){
		double argument = (correspondingPhysicalBaselines[b].dot(facetCenter.toVec()))*k; //this is -b.k_0
		baselineSpaceFactor[b].re = cos(argument);
		baselineSpaceFactor[b].im = sin(argument);
		//baselineSpaceFactor[b].re = 1;
		//baselineSpaceFactor[b].im = 0;
	}

	vector<double> pixelAreaAccumulated(nPixelsExtended, 0.0);

	for (int i = 0; i < nFull; i++){
		for (int j = 0; j < nFull; j++){
			double kx = (i - nFull/2.0) * deltak + kxOffset;
			double ky = (j - nFull/2.0) * deltak + kyOffset;
			if ((kx*kx + ky*ky) < (k*k)){ //otherwise it projects to outside the celestial sphere
				double kz = sqrt(k*k - kx*kx - ky*ky);
				cartVec thisPixelVector = arrayPrincipalAxes[0] * (kx/k) + arrayPrincipalAxes[1] * (ky/k) + arrayPrincipalAxes[2] * (kz/k);
				horizPoint thisHorizPoint = thisPixelVector.toHoriz();
				equaPoint thisEquaPoint = thisHorizPoint.toEqua(LST);
				int thisHealpixPixel = emptyMap.ang2pix(thisEquaPoint.toHealpixPointing());
				if (mapOfPixelsInExtendedFacet[thisHealpixPixel]){
					thisPixelsFacetCoordinate[i][j] = thisHealpixPixel;
					int thisHealpixIndex = extendedFacetPixelMapping[thisHealpixPixel];
					double PBfactor = primaryBeam(thisHorizPoint, PB);
					double geometryFactor = pow(deltak,2) / (k * fabs(kz)); 
					double realspaceFactor = PBfactor * geometryFactor * temperatureConversionFactor;
					pixelAreaAccumulated[thisHealpixIndex] = pixelAreaAccumulated[thisHealpixIndex] + geometryFactor;
					for (int b = 0; b < nSampled; b++){
						double argument = k * (thisPixelVector-(facetCenter.toVec())).dot(sampledBaselines[b]);
						complex complexPart(cos(argument),sin(argument));
						complexPart = complexPart * baselineSpaceFactor[b];
						Atranspose[thisHealpixIndex][b].re = Atranspose[thisHealpixIndex][b].re + complexPart.re * realspaceFactor;
						Atranspose[thisHealpixIndex][b].im = Atranspose[thisHealpixIndex][b].im + complexPart.im * realspaceFactor;
					}
				} 
			}
		}
	}

	cout << "NOT ADJUSTING FOR PIXEL AREA!!!" << endl;
	for (int n = 0; n < nPixelsExtended; n++){
		for (int b = 0; b < nSampled; b++){
			//Atranspose[n][b].re = Atranspose[n][b].re / pixelAreaAccumulated[n] * (4*pi / (12.0*NSIDE*NSIDE));
			//Atranspose[n][b].im = Atranspose[n][b].im / pixelAreaAccumulated[n] * (4*pi / (12.0*NSIDE*NSIDE));
		}
	}

	//Determine which pixels in the kx-ky plane map to which pixels in the facet
	//determine real space part for each kx-ky plane pixel
	
	return Atranspose;

}

//This function computes A^t matrix for each snapshot. The action of this matrix is to convert gridded visibilities--or unique baselines--into a dirty map.
vector< vector<complex> > calculateKAtranspose(double LST, vector<int>& extendedHealpixIndices, vector<equaPoint>& extendedPixelEquaPointings, vector<cartVec>& arrayPrincipalAxes, 
	vector<cartVec>& projectedBaselines, vector< vector<double> >& PB, horizPoint facetCenter, vector<cartVec>& sampledBaselines){

	//Calculate the real space diagonal part of Atranspose 
	//double deltak =  k * sqrt(4*pi / (12.0*NSIDE*NSIDE)) / mappingOverresolution;
	//double constantFactor = deltak * deltak / temperatureConversionFactor / k; //TODO: I MAY HAVE TO FIX THIS TO REPRESENT THE FACT THAT I'M DOING ONLY ONE PIXEL ON THE CURVED SKY AND NOT MANY PIXELS ON THE GROUND
	double constantFactor = 4*pi / (12.0*NSIDE*NSIDE) * temperatureConversionFactor; 
	vector<double> realSpaceDiagonalPart(nPixelsExtended, constantFactor);
	
	vector<horizPoint> extendedPixelHorizPointsAtThisLST;
	vector<cartVec> extendedPixelCartVecsAtThisLST;
	for (int n = 0; n < nPixelsExtended; n++){
		extendedPixelHorizPointsAtThisLST.push_back(extendedPixelEquaPointings[n].toHoriz(LST));
		extendedPixelCartVecsAtThisLST.push_back(extendedPixelHorizPointsAtThisLST[n].toVec());
		//realSpaceDiagonalPart[n] *= (primaryBeam(extendedPixelHorizPointsAtThisLST[n],PB) / fabs(arrayPrincipalAxes[2].dot(extendedPixelCartVecsAtThisLST[n])));
		realSpaceDiagonalPart[n] *= (primaryBeam(extendedPixelHorizPointsAtThisLST[n],PB));
	}
	
	//Calculate a the relevant Fourier transform matrix (A has e^ib.k, so A^t has e^-ib.k = e^i|k|b.theta_hat, as expressed mathematically here.)
	int nSampled = sampledBaselines.size();
	vector< vector<complex> > Atranspose(nPixelsExtended, vector<complex>(nSampled, complex(0,0)));
	for (int n = 0; n < nPixelsExtended; n++){
		for (int b = 0; b < nSampled; b++){		
			double argument = k * extendedPixelCartVecsAtThisLST[n].dot(sampledBaselines[b]);
			Atranspose[n][b].re = cos(argument) * realSpaceDiagonalPart[n];
			Atranspose[n][b].im = sin(argument) * realSpaceDiagonalPart[n];
		}
	}
	return Atranspose;
}

//This function computes KAtNinvAKt for each snapshot and then adds them to the existing sum to get an overall PSF
void addSnapshotPSF(vector< vector<double> >& PSF, vector< vector<complex> >& KAtranspose, vector<double>& Ninv, vector<cartVec>& sampledBaselines, vector<int>& mapOfIndicesInExtendedIndexVector){
	for (int n = 0; n < nPixels; n++){
		for (int nx = 0; nx < nPixelsExtended; nx++){
			for (int b = 0; b < sampledBaselines.size(); b++){
				PSF[n][nx] = PSF[n][nx] + (KAtranspose[nx][b] * KAtranspose[mapOfIndicesInExtendedIndexVector[n]][b].conj() * Ninv[b]).re;
			}
		}
	}
}

//For testing
void overwriteVisibilities(vector< vector<complex> >& allVisibilities, vector<cartVec>& baselines, vector< vector<double> >& PB, vector<double>& LSTs, vector<horizPoint> facetCenterPointings){
	equaPoint testPointSource = equaPoint(4.644893825717721, -0.4997050407593644);
	for (int l = 0; l < LSTs.size(); l++){
		horizPoint horizPointSource = testPointSource.toHoriz(LSTs[l]);
		double constantAtThisPoint = 1 * primaryBeam(horizPointSource, PB) * 4 * pi / (12 * NSIDE * NSIDE) ;
		for (int b = 0; b < baselines.size(); b++){
			double b_dot_k = baselines[b].dot(horizPointSource.toVec() * (-k));
			allVisibilities[b][l].re = constantAtThisPoint * cos(b_dot_k);
			allVisibilities[b][l].im = constantAtThisPoint * sin(b_dot_k);
		}
	}
	
}

//For testing
void performDirectMultiplication(vector<double>& directMultiplicationMap, vector< vector<complex> >& KAtranspose, vector< vector<complex> >& allVisibilities, vector<int>& LSTindices, 
	vector<int>& mapOfIndicesInExtendedIndexVector, vector<cartVec>& sampledBaselines, vector<cartVec>& correspondingPhysicalBaselines, vector< vector<double> >& PB, vector<double>& LSTs, vector<double>& Ninv, horizPoint facetCenter){
	
	int nSampled = sampledBaselines.size();


	vector<complex> baselineSpaceFactor(nSampled, complex(0,0));
	for (int b = 0; b < nSampled; b++){
		double argument = (correspondingPhysicalBaselines[b].dot(facetCenter.toVec()))*k; //this is -b.k_0
		baselineSpaceFactor[b].re = cos(argument);
		baselineSpaceFactor[b].im = sin(argument);
	}

	vector<complex> theseVisibilities(nSampled, complex(0,0));
	for (int l = 0; l < LSTindices.size(); l++){
		double thisLST = LSTs[LSTindices[l]];
		equaPoint testPointSource = equaPoint(4.644893825717721, -0.4997050407593644);
		//equaPoint testPointSource = equaPoint(4.632621979414636, -0.5235987755982991);
		horizPoint horizPointSource = testPointSource.toHoriz(thisLST);
		double constantAtThisPoint = 1 * primaryBeam(horizPointSource, PB) * 4 * pi / (12 * NSIDE * NSIDE) * temperatureConversionFactor;
		for (int b = 0; b < nSampled; b++){
			double b_dot_k = correspondingPhysicalBaselines[b].dot(horizPointSource.toVec() * (-k));
			//double rephasingFactor = correspondingPhysicalBaselines[b].dot(facetCenter.toVec())*k;
			//cout << b_dot_k << "  " << rephasingFactor << " " << b_dot_k + rephasingFactor << endl;
			theseVisibilities[b].re = theseVisibilities[b].re + constantAtThisPoint*cos(b_dot_k);
			theseVisibilities[b].im = theseVisibilities[b].im + constantAtThisPoint*sin(b_dot_k);
			/*if (b < 10){
				cout << "LST: " << LSTs[LSTindices[l]] << endl;
				cout << "Alt/Az:" << horizPointSource.alt << " " << horizPointSource.az << endl;
				cout << "Beam: " << primaryBeam(horizPointSource, PB) << endl;
				cout << "b_dot_k: " << correspondingPhysicalBaselines[b].dot(horizPointSource.toVec() * (-k)) << endl;
				cout << "Visibiltiy: " << constantAtThisPoint*cos(b_dot_k) << " "  << allVisibilities[int(floor(b/2.0))][LSTindices[l]].re << endl << endl;
			}*/
		}
	}


	/*for (int t = 0; t < nLSTs; t++){
		if (integrationsToUse[t]){
			for (int b = 0; b < nBaselines; b++){
				double b_dot_k = baselines[b].dot(facetCenterPointings[t].toVec())*(k); 
				complex complexFactor = complex(cos(b_dot_k), sin(b_dot_k)); //In A we multiply by e^b.k so in A^t we multiply by e^-b.k
				allVisibilities[b][t] = allVisibilities[b][t]*(complexFactor);
			}
		}
	}*/


	for (int b = 0; b < nSampled; b++){
		theseVisibilities[b] = theseVisibilities[b] * Ninv[b];
	}
cout << endl << "The rephased visibilities:" << endl;
	for (int b = 0; b < 5; b++){
		complex testVis = theseVisibilities[b]*baselineSpaceFactor[b];
		cout << setprecision(10) << b << " is at (" << sampledBaselines[b].x << ", " << sampledBaselines[b].y << ", " << sampledBaselines[b].z << ") with N^-1 = " << Ninv[b] << " and N^-1*y = " << testVis.re << " + " << testVis.im << "i. Abs = " << testVis.abs() <<  endl;
	}
/*	cout << endl << "The visibilities have not been rephased:" << endl;
		for (int b = 0; b < 5; b++){
		complex testVis = theseVisibilities[b];
		cout << setprecision(10) << b << " is at (" << sampledBaselines[b].x << ", " << sampledBaselines[b].y << ", " << sampledBaselines[b].z << ") with N^-1 = " << Ninv[b] << " and N^-1*y = " << testVis.re << " + " << testVis.im << "i. Abs = " << testVis.abs()  << endl;
	}*/
	cout << endl;



	for (int n = 0; n < nPixels; n++){
		for (int b = 0; b < nSampled; b++){
			directMultiplicationMap[n] = directMultiplicationMap[n] + (KAtranspose[mapOfIndicesInExtendedIndexVector[n]][b] * theseVisibilities[b]).re;
			//cout << directMultiplicationMap[n] << endl;
		}
	}


	/*for (int n = 0; n < nPixels; n++){
		for (int b = 0; b < nBaselines; b++){
			//complex visibilitySum(0,0);
			//for (int l = 0; l < LSTindices.size(); l++){
			//	visibilitySum = visibilitySum + allVisibilities[b][LSTindices[l]];
			//}
			//directMultiplicationMap[n] = directMultiplicationMap[n] + (KAtranspose[mapOfIndicesInExtendedIndexVector[n]][b*2] * visibilitySum).re;
			//directMultiplicationMap[n] = directMultiplicationMap[n] + (KAtranspose[mapOfIndicesInExtendedIndexVector[n]][b*2+1] * (visibilitySum.conj())).re;
			directMultiplicationMap[n] = directMultiplicationMap[n] + (KAtranspose[mapOfIndicesInExtendedIndexVector[n]][b*2] * theseVisibilities[b]).re;
			directMultiplicationMap[n] = directMultiplicationMap[n] + (KAtranspose[mapOfIndicesInExtendedIndexVector[n]][b*2+1] * (theseVisibilities[b].conj())).re;
		}
	}*/


}

//For testing
void createGriddedVisibilityFromTrueSky(vector< vector<complex> >& KAtranspose, vector< vector<complex> >& allVisibilities, vector<int>& LSTindices, 
	vector<int>& mapOfIndicesInExtendedIndexVector, vector<cartVec>& sampledBaselines, vector<cartVec>& correspondingPhysicalBaselines, vector< vector<double> >& PB, vector<double>& LSTs, vector<double>& Ninv, horizPoint facetCenter){

	vector<double> trueSky(nPixelsExtended, 0);
	trueSky[334] = 1;

	Healpix_Map<double> emptyMap = emptyHealpixMap();
	pointing pointSourcePointing = emptyMap.pix2ang(145530);
	equaPoint pointSourceEquaPoint(pointSourcePointing.phi, pi/2 - pointSourcePointing.theta);
	cout << setprecision(16) << pointSourceEquaPoint.ra << " " << pointSourceEquaPoint.dec << endl;
	



	int nSampled = sampledBaselines.size();
	vector<complex> griddedVisibilities(nSampled, complex(0,0));

	for (int n = 0; n < nPixelsExtended; n++){
		for (int b = 0; b < nSampled; b++){
			griddedVisibilities[b] = griddedVisibilities[b] + KAtranspose[n][b].conj() * trueSky[n];
		}
	}

	for (int b = 0; b < 5; b++){
		cout << setprecision(10) << b << " is at (" << sampledBaselines[b].x << ", " << sampledBaselines[b].y << ", " << sampledBaselines[b].z << ") with N^-1 = " << Ninv[b] << " and y = " << griddedVisibilities[b].re << " + " << griddedVisibilities[b].im << "i. Abs = " << griddedVisibilities[b].abs() << endl;
	}

}	



//This function normalizes the PSF to 1 at the desired point probed and it computes the diagonal normalization matrix needed to do that.
vector<double> computeNormalizationAndNormalizePSF(vector< vector<double> >& PSF, vector<int>& mapOfIndicesInExtendedIndexVector){
	cout << "Now normalizing the PSF..." << endl;
	vector<double> normalizationMatrix(nPixels,0.0);
	for (int n = 0; n < nPixels; n++){
		if (PSFpeaksAtOne) {
			double maxPSFValueInThisRow = 0.0;
			for (int nx = 0; nx < nPixelsExtended; nx++){
				if (PSF[n][nx] > maxPSFValueInThisRow) maxPSFValueInThisRow = PSF[n][nx];
			}
			normalizationMatrix[n] = 1.0/maxPSFValueInThisRow;
		} else {
			normalizationMatrix[n] = 1.0/PSF[n][mapOfIndicesInExtendedIndexVector[n]];	
		}	
	}
	for (int n = 0; n < nPixels; n++){
		for (int nx = 0; nx < nPixelsExtended; nx++) PSF[n][nx] = normalizationMatrix[n]*PSF[n][nx];
	}
	return normalizationMatrix;
}

//This function renormalizes the dirty map so that it is the true map convolved with the PSF (on average).
vector<double> renormalizeMap(vector<double>& normalizationMatrix, Healpix_Map<double>& coaddedMap, vector<int>& healpixIndices){
	vector<double> renormalizedMap(nPixels, 0.0);
	for (int n = 0; n < nPixels; n++) renormalizedMap[n] = coaddedMap[healpixIndices[n]] * normalizationMatrix[n];
	return renormalizedMap;
}

//This function computes the nosie covariance between pixels in the facet.
vector< vector<double> > computeNoiseCovariance(vector< vector<double> >& PSF, vector<double>& normalizationMatrix, vector<int>& mapOfIndicesInExtendedIndexVector){
	vector< vector<double> > noiseCovariance(nPixels, vector<double>(nPixels,0.0));
	for (int n1 = 0; n1 < nPixels; n1++){
		for (int n2 = 0; n2 < nPixels; n2++){
			noiseCovariance[n1][n2] = PSF[n1][mapOfIndicesInExtendedIndexVector[n2]] * normalizationMatrix[n2];
		}
	}
	return noiseCovariance;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// OUTPUT FUNCTIONS
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void exportMatrix(vector< vector<double> >& matrix, int dim1, int dim2, string filename){
	cout << "Now saving " << filename << " as a " << dim1 << " by " << dim2 << " matrix..." << endl;
	ofstream outfile;
	outfile.precision(16);	
	outfile.open(filename.c_str(), ios::trunc);		
	for (int i = 0; i < dim1; i++){
		for (int j = 0; j < dim2; j++){
			outfile << matrix[i][j] << " ";
		}
		outfile << endl;
	} 
	outfile.close();
}

void exportVector(vector<double>& vec, int dim, string filename){
	cout << "Now saving " << filename << " as a size " << dim << " vector..." << endl;
	ofstream outfile;
	outfile.precision(16);	
	outfile.open(filename.c_str(), ios::trunc);		
	for (int i = 0; i < dim; i++) outfile << vec[i] << endl;
	outfile.close();
}

void exportPixels(vector<int>& vec, string filename){
	cout << "Now saving " << filename << " as a list of  " << vec.size() << " healpix pixels..." << endl;
	ofstream outfile;
	outfile.open(filename.c_str(), ios::trunc);		
	for (int i = 0; i < vec.size(); i++) outfile << vec[i] << endl;
	outfile.close();
}

void exportPartialHealpixMap(Healpix_Map<double>& map, vector<int>&  healpixIndices, string filename){
	cout << "Now saving " << filename << " as a size " << healpixIndices.size() << " portion of a HEALPIX map..." << endl;
	ofstream outfile;
	outfile.precision(16);	
	outfile.open(filename.c_str(), ios::trunc);		
	for (int i = 0; i < healpixIndices.size(); i++) outfile << map[healpixIndices[i]] << endl;
	outfile.close();
}

void exportCoordinates(vector<equaPoint>& pixelCoordinates, string filename){
	cout << "Now saving " << filename << " as a list of  " << pixelCoordinates.size() << " healpix pixel equatorial coordinates..." << endl;
	ofstream outfile;
	outfile.precision(16);	
	outfile.open(filename.c_str(), ios::trunc);		
	for (int i = 0; i < pixelCoordinates.size(); i++) outfile << pixelCoordinates[i].ra << "     " << pixelCoordinates[i].dec << endl;
	outfile.close();
}

void exportComplexMatrix(vector< vector<complex> >& matrix, int dim1, int dim2, string filenameReal, string filenameImag){
	cout << "Now saving " << filenameReal << " as the " << dim1 << " by " << dim2 << " as the real matrix..." << endl;
	ofstream outfile;
	outfile.precision(16);	
	outfile.open(filenameReal.c_str(), ios::trunc);		
	for (int j = 0; j < dim2; j++){
		for (int i = 0; i < dim1; i++){
			outfile << matrix[i][j].re << " ";
		}
		outfile << endl;
	} 
	outfile.close();

	cout << "Now saving " << filenameImag << " as the " << dim1 << " by " << dim2 << " as the imaginary matrix..." << endl;
	ofstream outfile2;
	outfile2.precision(16);	
	outfile2.open(filenameImag.c_str(), ios::trunc);		
	for (int j = 0; j < dim2; j++){
		for (int i = 0; i < dim1; i++){
			outfile2 << matrix[i][j].im << " ";
		}
		outfile2 << endl;
	} 
	outfile2.close();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// MAIN FUNCTION
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int main(int argc, char *argv[]){
	
	cout << endl << "Running Faceted Mapmaking..." << endl << endl;
	//Load relevant quanties and data
	if (argc == 2) specsFilename = argv[1];
	loadSpecs();
	vector<int> baselineRedundancy;
	vector<cartVec> baselines = loadBaselines(baselineRedundancy);
	vector<cartVec> arrayPrincipalAxes = loadArrayPrincipalAxes();
	vector<cartVec> projectedBaselines = calculateProjectedBaselines(baselines,arrayPrincipalAxes);
	vector<double> LSTs; //LSTs of each integration
	vector< vector<complex> > allVisibilities = loadVisibilities(baselines, LSTs);
	vector< vector<double> > noiseVarianceOnEachVisibiltiy = loadNoiseVarianceOnEachVisibiltiy(baselines, LSTs, baselineRedundancy);
	vector<horizPoint> PBpointings = loadPBPointings(LSTs);
	vector< vector<double> > discretizedPrimaryBeam = loadDiscretizedPrimaryBeam();
	
	cout << endl << "PRIMARY BEAM SET TO 1" << endl << endl;

	//Geometric calculations 
	vector<horizPoint> facetCenterPointings = computeFacetCenterPointings(LSTs);
	vector<bool> integrationsToUse = determineIntegrationsToUse(PBpointings, facetCenterPointings);
	if (nIntegrationsToUse == 0) return 1;
	vector< vector<int> > snapshotLSTindices = assignLSTsToSnapshots(integrationsToUse);
	cout << "LSTs range from " << LSTs[snapshotLSTindices[0][0]] << " to " << LSTs[snapshotLSTindices[snapshotLSTindices.size()-1][snapshotLSTindices[snapshotLSTindices.size()-1].size()-1]] << "." << endl;
	
	vector<int> healpixIndices = computeHealpixIndices(false);
	vector<equaPoint> pixelEquaPointings = computeEquaPointings(healpixIndices);
	Healpix_Map<bool> mapOfPixelsInFacet = isInFacet(healpixIndices);
	exportPixels(healpixIndices,healpixPixelFilename);
	exportCoordinates(pixelEquaPointings, pixelCoordinatesFilename);
	
	vector<int> extendedHealpixIndices = computeHealpixIndices(true);
	vector<equaPoint> extendedPixelEquaPointings = computeEquaPointings(extendedHealpixIndices);
	Healpix_Map<bool> mapOfPixelsInExtendedFacet = isInFacet(extendedHealpixIndices);
	exportPixels(extendedHealpixIndices,extendedHealpixPixelFilename);
	exportCoordinates(extendedPixelEquaPointings, extendedPixelCoordiantesFilename);
	vector<int> mapOfIndicesInExtendedIndexVector = mapIndicesFromFacetToExtendedFacet(healpixIndices, extendedHealpixIndices);


	cout << endl << "OVERWRITING VISIBILITIES!!!" << endl << endl;
	overwriteVisibilities(allVisibilities, baselines, discretizedPrimaryBeam, LSTs, facetCenterPointings);

	//Rephase and renormalize
	convertToTemperatureUnits(allVisibilities, integrationsToUse);	
	noiseInverseVarianceWeight(allVisibilities, noiseVarianceOnEachVisibiltiy, integrationsToUse);
	vector< vector<complex> > unRephasedVisibilities = allVisibilities;
	rephaseVisibilities(allVisibilities, integrationsToUse, baselines, facetCenterPointings);
	
	cout << endl << "SETTING nSnapshots = 1!!!" << endl << endl;
	nSnapshots = 1;

	//Grid and FFT each snapshot
	cout << "Now making an unnormalized HEALPIX map..." << endl;
	Healpix_Map<double> coaddedMap = emptyHealpixMap();
	for (int n = 0; n < nSnapshots; n++){		
		cout << " " << floor(100.0 * n / nSnapshots) << "% done. \r" << std::flush;
		int snapshotCentralLSTindex = snapshotLSTindices[n][int(round(snapshotLSTindices[n].size()/2.0-.5))];
		vector< vector<complex> > uvGrid(nFull, vector<complex>(nFull, complex(0,0)));
		for (int i = 0; i < snapshotLSTindices[n].size(); i++){
			addVisibilitiesToGridInUV(uvGrid, projectedBaselines, arrayPrincipalAxes, facetCenterPointings, snapshotLSTindices[n][i], LSTs[snapshotLSTindices[n][i]], allVisibilities);
		}
		vector< vector<double> > snapshotMap = performUnnormalizedShiftedFFT(uvGrid, nFull, nFull, false);
		//if (n == 0) exportMatrix(snapshotMap, nFull, nFull, "snapshotMap.dat");
		addSnapshotMapToHealpixWithBeamAndGeometricFactors(snapshotMap, coaddedMap, mapOfPixelsInFacet, healpixIndices, arrayPrincipalAxes, discretizedPrimaryBeam, facetCenterPointings[snapshotCentralLSTindex], LSTs[snapshotCentralLSTindex]);
	}
	cout << "Done.                  " << endl;  
	exportPartialHealpixMap(coaddedMap, healpixIndices, unnormalizedMapFilename);
	
	//Calculate the cropped PSF
	cout << "Now calculating the position-dependent point spread function..." << endl;
	vector< vector<double> > PSF(nPixels, vector<double>(nPixelsExtended,0.0));
	vector<double> directMultiplicationMap(nPixels,0.0);
	for (int n = 0; n < nSnapshots; n++){		
		cout << " " << floor(100.0 * n / nSnapshots) << "% done. \r" << std::flush;
		vector<cartVec> sampledBaselines;
		vector<cartVec> correspondingPhysicalBaselines;
		vector<double> Ninv = calculateNinv(sampledBaselines, correspondingPhysicalBaselines, projectedBaselines, noiseVarianceOnEachVisibiltiy, arrayPrincipalAxes, snapshotLSTindices[n], LSTs);
		//for (int b = 0; b < Ninv.size(); b++) Ninv[b] = 1;
		//cout << "Ninv set to identity!!!" << endl;

		for (int b = 0; b < sampledBaselines.size(); b++){
			/*cout << "b = " << b << endl;
			sampledBaselines[b].print(); cout << endl;
			correspondingPhysicalBaselines[b].print(); cout << endl;*/
		}

		int snapshotCentralLSTindex = snapshotLSTindices[n][int(round(snapshotLSTindices[n].size()/2.0-.5))];
		vector< vector<complex> > KAtranspose = calculateKAtranspose(LSTs[snapshotCentralLSTindex], extendedHealpixIndices, extendedPixelEquaPointings, arrayPrincipalAxes, projectedBaselines, discretizedPrimaryBeam, facetCenterPointings[snapshotCentralLSTindex], sampledBaselines);
		

		vector< vector<complex> > KAtransposeVersion2 = calculateKAtransposeVersion2(LSTs[snapshotCentralLSTindex], extendedHealpixIndices, extendedPixelEquaPointings, arrayPrincipalAxes, projectedBaselines, discretizedPrimaryBeam, facetCenterPointings[snapshotCentralLSTindex], sampledBaselines, correspondingPhysicalBaselines, mapOfPixelsInExtendedFacet, mapOfPixelsInFacet);
		//KAtranspose = KAtransposeVersion2;

		addSnapshotPSF(PSF, KAtranspose, Ninv, sampledBaselines, mapOfIndicesInExtendedIndexVector); 
		performDirectMultiplication(directMultiplicationMap, KAtransposeVersion2, unRephasedVisibilities, snapshotLSTindices[n], mapOfIndicesInExtendedIndexVector, sampledBaselines, correspondingPhysicalBaselines, discretizedPrimaryBeam, LSTs, Ninv, facetCenterPointings[snapshotCentralLSTindex]);

		createGriddedVisibilityFromTrueSky(KAtranspose, unRephasedVisibilities, snapshotLSTindices[n], mapOfIndicesInExtendedIndexVector, sampledBaselines, correspondingPhysicalBaselines, discretizedPrimaryBeam, LSTs, Ninv, facetCenterPointings[snapshotCentralLSTindex]);
	}
	cout << "Done.                  " << endl;  
	vector<double> normalizationMatrix = computeNormalizationAndNormalizePSF(PSF, mapOfIndicesInExtendedIndexVector);
	exportVector(normalizationMatrix,nPixels,DmatrixFilename);

	//Compute and save the final data products
	exportMatrix(PSF, nPixels, nPixelsExtended, PSFfilename);
	vector<double> renormalizedMap = renormalizeMap(normalizationMatrix, coaddedMap, healpixIndices);
	exportVector(renormalizedMap, nPixels, finalMapFilename);
	vector< vector<double> > noiseCovariance = computeNoiseCovariance(PSF, normalizationMatrix, mapOfIndicesInExtendedIndexVector);
	exportMatrix(noiseCovariance, nPixels, nPixels, noiseCovFilename);

	for (int n = 0; n < nPixels; n++) directMultiplicationMap[n] = directMultiplicationMap[n] * normalizationMatrix[n];
	exportVector(directMultiplicationMap, nPixels, "directMultiplicationMap.dat");

	cout << "Done making a map. " << endl << endl;
	return 0;
}
