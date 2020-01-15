#include "Kalman.h"
#include <Windows.h>
#include <cmath>
#include <string>
#include <Ole2.h>
#include <gl/glew.h>
#include <iostream>
#include <thread>
#include <SFML/Audio.hpp>
#include <GL/freeglut.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <gl/glut.h>
#include <Kinect.h>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include "opencv2/video/tracking.hpp"
#include "opencv2/highgui.hpp"


//int counter = 0;//co 10 ramka wyswietla wspolrzedne
bool init(int argc, char* argv[]);
void execute();
void draw();
const int width = 512;
const int height = 424;
const int colorwidth = 1920;
const int colorheight = 1080;

// We'll be using buffer objects to store the kinect point cloud
GLuint vboId;
GLuint cboId;

// Intermediate Buffers
unsigned char rgbimage[colorwidth * colorheight * 4];    // Stores RGB color image
ColorSpacePoint depth2rgb[width * height];             // Maps depth pixels to rgb pixels
CameraSpacePoint depth2xyz[width * height];			 // Maps depth pixels to 3d coordinates

// Body tracking Variables
BOOLEAN tracked;							// Whether we see a body
Joint joints[JointType_Count];				// List of joints in the tracked body
cv::Mat_<float> filteredL(3,1);
cv::Mat_<float> filteredR(3,1);

// Kalman filter Variables
cv::KalmanFilter FilterL(6, 3, 0);
cv::KalmanFilter FilterR(6, 3, 0); 

// Kinect Variables
IKinectSensor* sensor;             // Kinect sensor
IMultiSourceFrameReader* reader;   // Kinect data source
ICoordinateMapper* mapper;         // Converts between depth, color, and 3d coordinates

// Drum Variables 
double SpeedR, SpeedL;				// Instantaneous speed of hands

sf::Sound left;						// SFML audio object for sound playing
sf::Sound right;

sf::SoundBuffer rimBuffer;			// Buffers storing wav files
sf::SoundBuffer snareBuffer; 
sf::SoundBuffer ophatBuffer;
sf::SoundBuffer clhatBuffer;

class instrument {

public:

	CameraSpacePoint centerPoint;
	sf::SoundBuffer buf;
	double height = 0.1;
	double width = 0.6;
	double length = 4;

	boolean recentlyPlayedR = 0;		// Boolean to be set after playing, and reset once hand leaves instrument area
	boolean recentlyPlayedL = 0;

	instrument(){
	}

	instrument(CameraSpacePoint center) {
		centerPoint = center;
	}

	void detectCollision(sf::Sound &left, sf::Sound &right, sf::SoundBuffer &buf) {
		if (tracked) {
			const CameraSpacePoint& LH = joints[JointType_HandLeft].Position;
			const CameraSpacePoint& RH = joints[JointType_HandRight].Position;

			if (recentlyPlayedL == 0) {
				if (SpeedL < 0)
					if ((filteredL(0) > centerPoint.X - width / 2) && (filteredL(0) < centerPoint.X + width / 2))
						if ((filteredL(1) < centerPoint.Y + height / 2) && (filteredL(1) > centerPoint.Y - height / 2))
							if ((filteredL(2) < centerPoint.Z + length / 2) && (filteredL(2) > centerPoint.Z - length / 2)) {
								left.setBuffer(buf);
								left.setVolume(abs(1000 * SpeedL * 200));
								left.setPitch(1);// - 0.2 * SpeedL));
								left.play();
								std::cout << "ZAGRANO LEWA Z PREDKOSCIA: " << SpeedL << std::endl;
								recentlyPlayedL = 1;
							}
			}
			else {
				if (filteredL(1) > centerPoint.Y + 0.6 * height)
					recentlyPlayedL = 0;
			}

			if (recentlyPlayedR == 0) {
				if (SpeedR < 0)
					if ((filteredR(0) > centerPoint.X - width / 2) && (filteredR(0) < centerPoint.X + width / 2))
						if ((filteredR(1) < centerPoint.Y + height / 2) && (filteredR(1) > centerPoint.Y - height / 2))
							if ((filteredR(2) < centerPoint.Z + length / 2) && (filteredR(2) > centerPoint.Z - length / 2)) {
								right.setBuffer(buf);
								right.setVolume(abs(1000 * SpeedR * 200));
								right.setPitch(1);// - 0.2 * SpeedR);
								right.play();
								std::cout << "ZAGRANO PRAWA" << std::endl;
								recentlyPlayedR = 1;
							}
			}
			else {
				if (filteredR(1) > centerPoint.Y + 0.6 * height)
					recentlyPlayedR = 0;
			}
		}
	}
}rim, kick, ophat, clhat;

bool initKinect() {
	if (FAILED(GetDefaultKinectSensor(&sensor))) {
		return false;
	}
	if (sensor) {
		sensor->get_CoordinateMapper(&mapper);

		sensor->Open();
		sensor->OpenMultiSourceFrameReader(
			FrameSourceTypes::FrameSourceTypes_Depth | FrameSourceTypes::FrameSourceTypes_Color | FrameSourceTypes::FrameSourceTypes_Body,
			&reader);
		return reader;
	}
	else {
		return false;
	}
}

void getDepthData(IMultiSourceFrame* frame, GLubyte* dest) {
	IDepthFrame* depthframe;
	IDepthFrameReference* frameref = NULL;
	frame->get_DepthFrameReference(&frameref);
	frameref->AcquireFrame(&depthframe);
	if (frameref) frameref->Release();

	if (!depthframe) return;

	// Get data from frame
	unsigned int sz;
	unsigned short* buf;
	depthframe->AccessUnderlyingBuffer(&sz, &buf);

	// Write vertex coordinates
	mapper->MapDepthFrameToCameraSpace(sz, buf, width * height, depth2xyz);
	float* fdest = (float*)dest;
	for (int i = 0; i < sz; i++) {
		*fdest++ = depth2xyz[i].X;
		*fdest++ = depth2xyz[i].Y;
		*fdest++ = depth2xyz[i].Z;
	}

	// Fill in depth2rgb map
	mapper->MapDepthFrameToColorSpace(sz, buf, width * height, depth2rgb);
	if (depthframe) depthframe->Release();
}

void getRgbData(IMultiSourceFrame* frame, GLubyte* dest) {
	IColorFrame* colorframe;
	IColorFrameReference* frameref = NULL;
	frame->get_ColorFrameReference(&frameref);
	frameref->AcquireFrame(&colorframe);
	if (frameref) frameref->Release();

	if (!colorframe) return;

	// Get data from frame
	colorframe->CopyConvertedFrameDataToArray(colorwidth * colorheight * 4, rgbimage, ColorImageFormat_Rgba);

	// Write color array for vertices
	ColorSpacePoint* p = depth2rgb;
	float* fdest = (float*)dest;
	for (int i = 0; i < height; i++) {
		for (int j = 0; j < width; j++) {
			if (p->X < 0 || p->Y < 0 || p->X > colorwidth || p->Y > colorheight) {
				*fdest++ = 0;
				*fdest++ = 0;
				*fdest++ = 0;
			}
			else {
				int idx = (int)p->X + colorwidth * (int)p->Y;
				*fdest++ = rgbimage[4 * idx + 0] / 255.;
				*fdest++ = rgbimage[4 * idx + 1] / 255.;
				*fdest++ = rgbimage[4 * idx + 2] / 255.;
			}
			// Don't copy alpha channel
			p++;
		}
	}

	if (colorframe) colorframe->Release();
}

void getBodyData(IMultiSourceFrame* frame) {
	IBodyFrame* bodyframe;
	IBodyFrameReference* frameref = NULL;
	frame->get_BodyFrameReference(&frameref);
	frameref->AcquireFrame(&bodyframe);
	if (frameref) frameref->Release();

	if (!bodyframe) return;

	IBody* body[BODY_COUNT] = { 0 };
	bodyframe->GetAndRefreshBodyData(BODY_COUNT, body);
	for (int i = 0; i < BODY_COUNT; i++) {
		body[i]->get_IsTracked(&tracked);
		if (tracked) {
			body[i]->GetJoints(JointType_Count, joints);
			break;
		}
	}

	if (bodyframe) bodyframe->Release();
}

void getKinectData() {
	IMultiSourceFrame* frame = NULL;
	if (SUCCEEDED(reader->AcquireLatestFrame(&frame))) {
		GLubyte* ptr;
		glBindBuffer(GL_ARRAY_BUFFER, vboId);
		ptr = (GLubyte*)glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
		if (ptr) {
			getDepthData(frame, ptr);
		}
		glUnmapBuffer(GL_ARRAY_BUFFER);
		glBindBuffer(GL_ARRAY_BUFFER, cboId);
		ptr = (GLubyte*)glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
		if (ptr) {
			getRgbData(frame, ptr);
		}
		glUnmapBuffer(GL_ARRAY_BUFFER);
		getBodyData(frame);
	}
	if (frame) frame->Release();
}

void rotateCamera() {
	static double angle = 0.;
	static double radius = 3.;
	double x = radius * sin(angle);
	double z = radius * (1 - cos(angle)) - radius / 2;
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	gluLookAt(x, 0, z, 0, 0, radius / 2, 0, 1, 0);
	angle += 0.001;
}

void drawInstruments() {
	glVertex3f(rim.centerPoint.X - rim.width / 2, rim.centerPoint.Y + rim.height / 2, rim.centerPoint.Z);
	glVertex3f(rim.centerPoint.X + rim.width / 2, rim.centerPoint.Y + rim.height / 2, rim.centerPoint.Z);

	glVertex3f(kick.centerPoint.X - kick.width / 2, kick.centerPoint.Y + kick.height / 2, kick.centerPoint.Z);
	glVertex3f(kick.centerPoint.X + kick.width / 2, kick.centerPoint.Y + kick.height / 2, kick.centerPoint.Z);

	glVertex3f(clhat.centerPoint.X - clhat.width / 2, clhat.centerPoint.Y + clhat.height / 2, clhat.centerPoint.Z);
	glVertex3f(clhat.centerPoint.X + clhat.width / 2, clhat.centerPoint.Y + clhat.height / 2, clhat.centerPoint.Z);

	glVertex3f(ophat.centerPoint.X - ophat.width / 2, ophat.centerPoint.Y + ophat.height / 2, ophat.centerPoint.Z);
	glVertex3f(ophat.centerPoint.X + ophat.width / 2, ophat.centerPoint.Y + ophat.height / 2, ophat.centerPoint.Z);
}

void drawKinectData() {
	getKinectData();
	//rotateCamera();

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);

	glBindBuffer(GL_ARRAY_BUFFER, vboId);//
	glVertexPointer(3, GL_FLOAT, 0, NULL);

	glBindBuffer(GL_ARRAY_BUFFER, cboId);//
	glColorPointer(3, GL_FLOAT, 0, NULL);

	glPointSize(1.f);
	glDrawArrays(GL_POINTS, 0, width * height);

	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);

	if (tracked) {

		const CameraSpacePoint& lw = joints[JointType_WristLeft].Position;
		const CameraSpacePoint& le = joints[JointType_ElbowLeft].Position;;
		const CameraSpacePoint& ls = joints[JointType_ShoulderLeft].Position;
		const CameraSpacePoint& rw = joints[JointType_WristRight].Position;
		const CameraSpacePoint& re = joints[JointType_ElbowRight].Position;
		const CameraSpacePoint& rs = joints[JointType_ShoulderRight].Position;
		const CameraSpacePoint& rh = joints[JointType_HandRight].Position;
		const CameraSpacePoint& lh = joints[JointType_HandLeft].Position;
		const CameraSpacePoint& lht = joints[JointType_HandTipLeft].Position;
		const CameraSpacePoint& rht = joints[JointType_HandTipRight].Position;
		const CameraSpacePoint& lt = joints[JointType_ThumbLeft].Position;
		const CameraSpacePoint& rt = joints[JointType_ThumbRight].Position;
		const CameraSpacePoint& neck = joints[JointType_Neck].Position;


		//if (counter == 10) {
		//	std::cout << "os Z: " << ls.Z - lht.Z << " os Y:" << ls.Y - lht.Y << " os X: " << ls.X - lht.X << std::endl;
		//	std::cout << "os Z: " << lh.Z << " os Y:" << lh.Y << " os X: " <<  lh.X << std::endl;
			Joint joint = joints[JointType_HandLeft];
			if (joint.TrackingState == TrackingState_Inferred)
				std::cout << "inferred" << std::endl;
		//	counter = 0;
		//}
		//counter++;
		//std::cout << "os Z: " << rht.Z << " os Y:" << rht.Y << " os X: " << rht.X << std::endl;

		glLineWidth(5);
		glBegin(GL_LINES);
		glColor3f(1.f, 0.f, 0.f);
	
		//glVertex3f(lht.X, lht.Y, lht.Z);
		//glVertex3f(lh.X, lh.Y, lh.Z);

		//glVertex3f(lh.X, lh.Y, lh.Z);
		//glVertex3f(lt.X, lt.Y, lt.Z);

		glVertex3f(filteredL(0), filteredL(1), filteredL(2));
		glVertex3f(lw.X, lw.Y, lw.Z);

		glVertex3f(lw.X, lw.Y, lw.Z);
		glVertex3f(le.X, le.Y, le.Z);

		glVertex3f(le.X, le.Y, le.Z);
		glVertex3f(ls.X, ls.Y, ls.Z);

		//glVertex3f(rht.X, rht.Y, rht.Z);
		//glVertex3f(rh.X, rh.Y, rh.Z);

		//glVertex3f(rh.X, rh.Y, rh.Z);
		//glVertex3f(rt.X, rt.Y, rt.Z);
		
		glVertex3f(filteredR(0), filteredR(1), filteredR(2));
		glVertex3f(rw.X, rw.Y, rw.Z);

		glVertex3f(rw.X, rw.Y, rw.Z);
		glVertex3f(re.X, re.Y, re.Z);

		glVertex3f(re.X, re.Y, re.Z);
		glVertex3f(rs.X, rs.Y, rs.Z);

		//instrument area 
		drawInstruments();
		
		glEnd();

	}
	else {
	std::cout << "body untracked" << std::endl;
	}
	
}



void measureSpeed(double &SpeedR, double &SpeedL)
{
	static double prevR = filteredR(1);
	static double prevL = filteredL(1);
	static std::chrono::high_resolution_clock::time_point prevTime = std::chrono::high_resolution_clock::now();

	double LH = filteredL(1);
	double RH = filteredR(1);

	std::chrono::high_resolution_clock::time_point currentTime = std::chrono::high_resolution_clock::now();
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - prevTime).count();

	if (LH != prevL)
		if(elapsed != 0)
			SpeedL = (LH - prevL) / (elapsed);

	if (RH != prevR)
		if (elapsed != 0)
			SpeedR = (RH - prevR) / (elapsed);

	//std::cout <<  "SpeedR: " << std::setprecision(5) <<  10000 * SpeedR << " SpeedL: " << std::setprecision(5) << 10000 * SpeedL << std::endl;

	prevR = filteredR(1);
	prevL = filteredL(1);
	prevTime = std::chrono::high_resolution_clock::now();
}


void initializeGL() {

	// OpenGL setup
	glClearColor(0, 0, 0, 0);
	glClearDepth(1.0f);

	// Set up array buffers
	const int dataSize = width * height * 3 * 4;
	glGenBuffers(1, &vboId);
	glBindBuffer(GL_ARRAY_BUFFER, vboId);
	glBufferData(GL_ARRAY_BUFFER, dataSize, 0, GL_DYNAMIC_DRAW);
	glGenBuffers(1, &cboId);
	glBindBuffer(GL_ARRAY_BUFFER, cboId);
	glBufferData(GL_ARRAY_BUFFER, dataSize, 0, GL_DYNAMIC_DRAW);

	// Camera setup
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(45, width / (GLdouble)height, 0.1, 1000);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	gluLookAt(0, 0, 0, 0, 0, 1, 0, 1, 0);

}

void draw() {
	drawKinectData();
	measureSpeed(SpeedR, SpeedL);

	filteredL = kalman(joints[JointType_HandLeft].Position.X, joints[JointType_HandLeft].Position.Y, joints[JointType_HandLeft].Position.Z, FilterL);
	filteredR = kalman(joints[JointType_HandRight].Position.X, joints[JointType_HandRight].Position.Y, joints[JointType_HandRight].Position.Z, FilterR);

	rim.detectCollision(left, right, rimBuffer);
	kick.detectCollision(left, right, snareBuffer);
	ophat.detectCollision(left, right, ophatBuffer);
	clhat.detectCollision(left, right, clhatBuffer);

	glutSwapBuffers();
}
#if 0
void mojexecute() {
	while (1) {
		drawKinectData();
		measureSpeed(SpeedR, SpeedL);

		rim.detectCollision(left, right, rimBuffer);
		kick.detectCollision(left, right, snareBuffer);
		ophat.detectCollision(left, right, ophatBuffer);
		clhat.detectCollision(left, right, clhatBuffer);

		glutSwapBuffers();
	}
}

#endif // 0


void execute() {
	glutMainLoop();
}

bool init(int argc, char* argv[]) {
	glutInit(&argc, argv);
	glutInitDisplayMode(GLUT_DEPTH | GLUT_DOUBLE | GLUT_RGBA);
	glutInitWindowSize(width, height);
	glutCreateWindow("Kinect SDK Tutorial");
	glutDisplayFunc(draw);
	glutIdleFunc(draw);
	glewInit();
	return true;
}

int main(int argc, char* argv[]) {
	if (!init(argc, argv)) return 1;
	if (!initKinect()) return 1;
	initializeGL();

	rimBuffer.loadFromFile("rim.wav");
	snareBuffer.loadFromFile("kick.wav");
	ophatBuffer.loadFromFile("ophat.wav");
	clhatBuffer.loadFromFile("clhat.wav");

	kick.centerPoint.X = 0.58;
	kick.centerPoint.Y = -0.33;
	kick.centerPoint.Z = 1.22;

	rim.centerPoint.X = -0.42;
	rim.centerPoint.Y = -0.33;
	rim.centerPoint.Z = 1.22;

	clhat.centerPoint.X = 0.7;
	clhat.centerPoint.Y = 0.02;
	clhat.centerPoint.Z = 1.1;

	ophat.centerPoint.X = -0.55;
	ophat.centerPoint.Y = 0.02;
	ophat.centerPoint.Z = 1.1;


	kalman_init(joints[JointType_HandLeft].Position.X, joints[JointType_HandLeft].Position.Y, joints[JointType_HandLeft].Position.Z, FilterL);
	kalman_init(joints[JointType_HandRight].Position.X, joints[JointType_HandRight].Position.Y, joints[JointType_HandRight].Position.Z, FilterR);

	// Main loop
	execute();
	return 0;
}