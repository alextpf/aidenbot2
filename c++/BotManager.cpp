
#include "BotManager.h"
#include "Utility.h"
#include "../arduino/aidenbot/Configuration.h"

#include <conio.h> // for getch
#include <windows.h> // for kbhit

#define PI							3.1415926
#define DEG_TO_RAD					PI / 180.0f

// color definition
#define BLUE   cv::Scalar( 255,   0,   0 ) //BGR
#define GREEN  cv::Scalar(   0, 255,   0 )
#define RED    cv::Scalar(   0,   0, 255 )
#define YELLOW cv::Scalar(   0, 255, 255 )
#define WHITE  cv::Scalar( 255, 255, 255 )
#define BLACK  cv::Scalar(   0,   0,   0 )
#define PURPLE cv::Scalar( 255, 112, 132 )
#define MEDIUM_PURPLE cv::Scalar( 219, 112, 147 )
#define CORNER_WIN "corners"

//#define DEBUG_SERIAL

//=======================================================================
// helper function to show type
std::string type2str(int type)
{
	std::string r;

	uchar depth = type & CV_MAT_DEPTH_MASK;
	uchar chans = 1 + (type >> CV_CN_SHIFT);

	switch (depth) {
	case CV_8U:  r = "8U"; break;
	case CV_8S:  r = "8S"; break;
	case CV_16U: r = "16U"; break;
	case CV_16S: r = "16S"; break;
	case CV_32S: r = "32S"; break;
	case CV_32F: r = "32F"; break;
	case CV_64F: r = "64F"; break;
	default:     r = "User"; break;
	}

	r += "C";
	r += (chans + '0');

	return r;
}//std::string type2str(int type)

//=======================================================================
BotManager::BotManager( char* com )
: m_TableFound( false )
, m_BandWidth( 0 )
, m_ShowDebugImg( false )
, m_ShowOutPutImg( true )
, m_ManualPickTableCorners( false )
, m_NumFrame( 0 )
, m_NumConsecutiveNonPuck( 0 )
, m_CorrectMissingSteps( false )
{
	m_FpsCalculator.SetBufferSize( 10 );
	m_pSerialPort = std::make_shared<SerialPort>( com );
}

//=======================================================================
void BotManager::Process(cv::Mat & input, cv::Mat & output)
{
	// check if keyboard "h/H" is hit
	if ( m_Debug )
	{
		TestMotion();
		return;
	}

	//if ( m_ShowOutPutImg )
	{
		output = input.clone();
	}

	// find table range
	if ( !m_TableFound )
	{
		FindTable( input );
	} // if ( !m_TableFound )

	if ( m_TableFound )
	{
		cv::destroyWindow( CORNER_WIN );

		if ( m_ShowOutPutImg )
		{
			// draw table boundary
			cv::Point TopLeft( static_cast<int>( m_TableFinder.GetLeft() ), static_cast<int>( m_TableFinder.GetTop() ) );
			cv::Point TopRight( static_cast<int>( m_TableFinder.GetRight() ), static_cast<int>( m_TableFinder.GetTop() ) );
			cv::Point LowerLeft( static_cast<int>( m_TableFinder.GetLeft() ), static_cast<int>( m_TableFinder.GetBottom() ) );
			cv::Point LowerRight( static_cast<int>( m_TableFinder.GetRight() ), static_cast<int>( m_TableFinder.GetBottom() ) );

			cv::line( output, TopLeft, TopRight, GREEN, 2 );
			cv::line( output, TopLeft, LowerLeft, GREEN, 2 );
			cv::line( output, TopRight, LowerRight, GREEN, 2 );
			cv::line( output, LowerLeft, LowerRight, GREEN, 2 );
		}

		// get time stamp
		clock_t curr = clock();

		unsigned int dt = static_cast<unsigned int>( ( curr - m_CurrTime ) * 1000.0f / CLOCKS_PER_SEC ); // in ms

		//debug
		//dt = 50; // ms, 20 FPS, for debug purpose

		// calculate FPS
		m_FpsCalculator.AddFrameTime( dt );
		unsigned int fps = m_FpsCalculator.GetFPS();
		//===========================================

		// find puck and robot position

		cv::Point detectedPuckPos;

		// convert RBG to HSV first
		cv::Mat hsvImg;
		cv::cvtColor( input, hsvImg, CV_BGR2HSV );

		//1. find robot
		cv::Point detectedBotPos( -1, -1 );
		const bool botFound = FindRobot( detectedBotPos, hsvImg, output, dt );

		// 2. find puck
		bool bailOut = false;

		cv::Point prevPuckPos;
		cv::Point predPuckPos;
		cv::Point bouncePos;
		cv::Point desiredBotPos;

		const bool puckFound = FindPuck( detectedPuckPos, hsvImg, output, dt, fps,
            bailOut, prevPuckPos, predPuckPos, bouncePos, desiredBotPos );

        // 3. decide whether to correct missing steps
        const bool correctSteps = CorrectMissingSteps( botFound );

		// 3. send message over serial port to Arduino
		if ( puckFound && !bailOut )
		{
			// send the message by com port over to Arduino
			SendBotMessage( correctSteps );
#ifdef DEBUG_SERIAL
			ReceiveMessage();
#endif // DEBUG
		}

		if ( m_IsLog )
		{
			if ( !puckFound )
			{
				detectedPuckPos = cv::Point( -1, -1 );
			}

			m_Logger.LogStatus(
				m_NumFrame, dt, fps,
				detectedPuckPos, // img coordinate
				bouncePos, // img coordinate
				predPuckPos, // img coordinate
				prevPuckPos, // img coordinate
				desiredBotPos, // img coordinate
				detectedBotPos, // img coordinate
				m_Camera.GetCurrPuckSpeed(),
				m_Camera.GetPredictTimeDefence(), // ms
				m_Camera.GetPredictTimeAttack(), // ms
                m_Camera.GetPredictTimeAtBounce(), // ms
				m_Camera.GetCurrNumPredictBounce(),
                m_Robot.GetDesiredRobotXSpeed(),
				m_Robot.GetDesiredRobotYSpeed(),
				m_Camera.GetPredictStatus(),
				m_Robot.GetRobotStatus(),
				m_Robot.GetAttackStatus(),
				m_Robot.GetAttackTime(),
				m_Camera.GetPuckAvgSpeed(),
                m_CorrectMissingSteps,
				bailOut);
		}

		// update time stamp and puck position
		m_CurrTime = curr;

	} // if ( m_TableFound )

	// increment frame number
	m_NumFrame++;

}//Process

//=======================================================================
bool BotManager::CorrectMissingSteps( const bool botFound )
{
    bool tmp = false;

    if( m_CurrTime > 0 && botFound )
    {
        int speedThresh = 30;
        int posErr = 10;

        cv::Point2f& currSpeed = m_Camera.GetCurrBotSpeed();
        cv::Point2f& prevSpeed = m_Camera.GetPrevBotSpeed();

        cv::Point& currPos = m_Camera.GetCurrBotPos();
        cv::Point& predictPos = m_Robot.GetDesiredRobotPos();
        cv::Point posDif = predictPos - currPos;

        tmp =   std::abs( currSpeed.x ) < speedThresh &&
                std::abs( currSpeed.y ) < speedThresh &&
                std::abs( prevSpeed.x ) < speedThresh &&
                std::abs( prevSpeed.y ) < speedThresh &&
                std::abs( posDif.x ) < posErr         &&
                std::abs( posDif.y ) < posErr;
    } //if( m_CurrTime > 0 )

    return m_CorrectMissingSteps && tmp;
}//CorrectMissingSteps

//=======================================================================
void BotManager::TestMotion()
{
	if ( _kbhit() )
	{
        m_Robot.SetDesiredRobotYSpeed( static_cast<int>( MAX_Y_ABS_SPEED * 0.7f ) );
        m_Robot.SetDesiredRobotXSpeed( static_cast<int>( MAX_X_ABS_SPEED * 0.7f ) );

		int key = _getch();
		switch ( key )
		{
		case 49://"1"
			m_Robot.SetDesiredRobotPos( cv::Point( ROBOT_MAX_X, ROBOT_MAX_Y ) );
			SendBotMessage( false );
			break;
		case 50://"2"
			m_Robot.SetDesiredRobotPos( cv::Point( ROBOT_MIN_X, ROBOT_MAX_Y ) );
			SendBotMessage( false );
			break;
		case 51://"3"
			m_Robot.SetDesiredRobotPos( cv::Point( ROBOT_MIN_X, ROBOT_MIN_Y ) );
			SendBotMessage( false );
			break;
		case 52://"4"
			m_Robot.SetDesiredRobotPos( cv::Point( ROBOT_MAX_X, ROBOT_MIN_Y ) );
			SendBotMessage( false );
			break;
		case 53://"5"
			m_Robot.SetDesiredRobotPos( cv::Point( ROBOT_CENTER_X, ROBOT_MAX_Y ) );
			SendBotMessage( false );
			break;
		case 54://"6"
			m_Robot.SetDesiredRobotPos( cv::Point( ROBOT_CENTER_X, ROBOT_MIN_Y ) );
			SendBotMessage( false );
			break;
		case 55://"7"
			m_Robot.SetDesiredRobotPos( cv::Point( ROBOT_MAX_X, ROBOT_DEFENSE_ATTACK_POSITION_DEFAULT ) );
			SendBotMessage( false );
			break;
		case 56://"8"
			m_Robot.SetDesiredRobotPos( cv::Point( ROBOT_MIN_X, ROBOT_DEFENSE_ATTACK_POSITION_DEFAULT ) );
			SendBotMessage( false );
			break;
		default:
			break;
		}
	}
}//TestMotion

//=======================================================================
bool BotManager::FindRobot(
	cv::Point& detectedBotPos,
	const cv::Mat& hsvImg,
	cv::Mat & output,
	const unsigned int dt)
{
	Contours contours;

    bool botFound = m_BotFinder.FindDisk1Thresh(
        contours, detectedBotPos, hsvImg, m_BlueThresh, m_Mask );

	if ( botFound )
	{
		// Show detected robot position
		if ( m_ShowOutPutImg )
		{
			const int radius = 15;
			const int thickness = 2;
			cv::circle( output, detectedBotPos, radius, RED, thickness );
		}

		// convert screen coordinate to table coordinate
		const cv::Point botPos = m_TableFinder.ImgToTableCoordinate( detectedBotPos ); // mm, in table coordinate

		// robot should be within its range
        const int tolerance = 10; // 10 mm tolerance

		if ( botPos.x < ROBOT_MIN_X - tolerance || botPos.x > ROBOT_MAX_X + tolerance ||
		   	 botPos.y < ROBOT_MIN_Y - tolerance || botPos.y > ROBOT_MAX_Y + tolerance )
		{
			// detected is noise
            botFound = false;
		}
		else
		{
			m_Camera.SetCurrBotPos( botPos );

            // calculate and set speed
            m_Camera.SetPrevBotSpeed( m_Camera.GetCurrBotSpeed() );

            cv::Point posDif = m_Camera.GetCurrBotPos() - m_Camera.GetPrevBotPos();
            const cv::Point2f tmp = static_cast<cv::Point2f>( posDif * 100 );
            m_Camera.SetCurrBotSpeed( tmp / static_cast<int>( dt ) ); // speed in dm/ms (we use this units to not overflow the variable)

            m_Camera.SetPrevBotPos( botPos );
		}
	} // if( botFound )

	return botFound;
}// FindRobot

//=======================================================================
bool BotManager::FindPuck(
	cv::Point& detectedPuckPos,
	const cv::Mat& hsvImg,
	cv::Mat & output,
	const unsigned int dt,
	const unsigned int fps,
	bool& bailOut,
	cv::Point& prevPuckPos,
	cv::Point& predPuckPos,
	cv::Point& bouncePos,
	cv::Point& desiredBotPos )
{
	Contours contours;

	const bool puckFound = m_PuckFinder.FindDisk2Thresh(
		contours, detectedPuckPos, hsvImg, m_RedThresh, m_OrangeThresh, m_Mask );

	if ( puckFound )
	{
		// std::cout << "can't find puck" << std::endl;
		//draw puck center
		if ( m_ShowOutPutImg )
		{
			const int radius = 15;
			const int thickness = 2;
			cv::circle( output, detectedPuckPos, radius, GREEN, thickness );
		}

		// convert screen coordinate to table coordinate
		const cv::Point puckPos = m_TableFinder.ImgToTableCoordinate( detectedPuckPos ); // mm, in table coordinate

		m_Camera.SetCurrPuckPos( puckPos );

		//ownGoal = m_Robot.IsOwnGoal( m_Camera );

		// skip processing if 1st frame

		if ( /*!ownGoal &&*/ /*dt < 2000 &&*/ m_CurrTime > 0 )
		{
			if ( m_NumConsecutiveNonPuck > 1 )
			{
				m_Camera.SetPrevPuckPos( cv::Point( 0, 0 ) ); // reset
			}

			// do prediction work
            m_Camera.CamProcess( dt, m_Camera.GetCurrBotPos() /* table coord*/ );

			prevPuckPos = m_Camera.GetPrevPuckPos(); // mm, table coordinate
			prevPuckPos = m_TableFinder.TableToImgCoordinate( prevPuckPos );

			predPuckPos = m_Camera.GetCurrPredictPos();
			predPuckPos = m_TableFinder.TableToImgCoordinate( predPuckPos );

			bouncePos = m_Camera.GetBouncePos();
			const bool isBounce = bouncePos.x != -1;
			if ( isBounce )
			{
				bouncePos = m_TableFinder.TableToImgCoordinate( bouncePos );
			}

			if ( m_ShowOutPutImg )
			{
				// show FPS on screen
				const std::string text = "FPS = " + std::to_string( fps );
				cv::Point origin( 520, 25 ); // upper right
				int thickness = 1;
				int lineType = 8;

				cv::putText( output, text, origin, cv::FONT_HERSHEY_SIMPLEX, 0.5, MEDIUM_PURPLE, thickness, lineType );

				// draw previous pos

				cv::line( output, prevPuckPos, detectedPuckPos, cv::Scalar( 130, 221, 238 ), 2 );

				// draw prediction on screen
				Camera::PREDICT_STATUS predictStatus = m_Camera.GetPredictStatus();
				if ( predictStatus == Camera::PREDICT_STATUS::DIRECT_IMPACT ||
					predictStatus == Camera::PREDICT_STATUS::ONE_BOUNCE )
				{
					// draw bounce pos if there's one
					if ( !isBounce )
					{
						// no bounce
						cv::line( output, predPuckPos, detectedPuckPos, PURPLE, 2 );
					}
					else
					{
						// have one bounce
						cv::line( output, bouncePos, detectedPuckPos, PURPLE, 2 );
						cv::line( output, predPuckPos, bouncePos, PURPLE, 2 );
					}
				}
			}//if ( m_ShowOutPutImg )

			 // determine robot strategy
			m_Robot.NewDataStrategy( m_Camera );

			// determine robot position
            bailOut = m_Robot.RobotMoveDecision( m_Camera ); // determins m_DesiredRobotPos

            if( !bailOut )
            {
                desiredBotPos = m_Robot.GetDesiredRobotPos();
                desiredBotPos = m_TableFinder.TableToImgCoordinate( desiredBotPos );

                if( m_ShowOutPutImg )
                {
                    // Show desired robot position
                    const int radius = 5;
                    const int thickness = 2;
                    cv::circle( output, desiredBotPos, radius, BLUE, thickness );
                }//if ( m_ShowOutPutImg )
            }
		} // if ( /*dt < 2000 &&*/ m_CurrTime > 0 )

		m_Camera.SetPrevPuckPos( puckPos );

		m_NumConsecutiveNonPuck = 0;
	}
	else
	{
		// puck not found
		m_NumConsecutiveNonPuck++;
	} // if( puckFound )

	return puckFound;
}//FindPuck

//=======================================================================
void BotManager::FindTable( cv::Mat & input )
{
	cv::Point TopLeft;
	cv::Point TopRight;
	cv::Point LowerLeft;
	cv::Point LowerRight;

	cv::imshow( CORNER_WIN, input );
	cv::setMouseCallback( CORNER_WIN, OnMouse, &m_Corners );

	// user-picked 4 corners
	while ( m_Corners.size() < 4 )
	{
		size_t m = m_Corners.size();
		if ( m > 0 )
		{
			cv::circle( input, m_Corners[m - 1], 3, GREEN, 2 );
		}

		cv::imshow( CORNER_WIN, input );
		cv::waitKey( 10 );
	}

	// last point
	cv::circle( input, m_Corners[3], 3, GREEN, 2 );

	cv::imshow( CORNER_WIN, input );
	cv::waitKey( 10 );
	// order the 4 corners
	OrderCorners();

	if ( m_ShowDebugImg )
	{
		// draw the bands around 4 picked corners
		// m_Corners is arranged by: ul, ur, ll, lr
		cv::circle( input, m_Corners[0], 3, GREEN, 2 );
		cv::circle( input, m_Corners[1], 3, RED, 2 );
		cv::circle( input, m_Corners[2], 3, BLUE, 2 );
		cv::circle( input, m_Corners[3], 3, WHITE, 2 );

		cv::imshow( "ORder:", input );
	} // DEBUG

	if ( !m_ManualPickTableCorners )
	{
		// blur image first by Gaussian
		int kernelSize = 3;
		double std = 2.0;

		cv::GaussianBlur( input, input, cv::Size( kernelSize, kernelSize ), std, std );

		if ( m_ShowDebugImg )
		{
			cv::imshow( "gauss:", input );
		} // DEBUG

		  // convert to gray scale
		cv::Mat tmp;
		cv::cvtColor( input, tmp, cv::COLOR_RGB2GRAY );

		cv::Mat tmp2 = tmp.clone();

		// adaptive threshold
		int adaptiveMethod = cv::ADAPTIVE_THRESH_MEAN_C;
		int thresholdType = cv::THRESH_BINARY;
		int blockSiz = 55;

		cv::adaptiveThreshold( tmp2, tmp2, 255, adaptiveMethod, thresholdType, blockSiz, 5 );
		//cv::threshold(tmp, tmp1, 128, 255, cv::THRESH_BINARY_INV);

		// close, fill the "holes" in the foreground
		//cv::dilate( tmp2, tmp2, cv::Mat(), cv::Point( -1, -1 ), 2/*num iteration*/ );
		//      cv::erode( tmp2, tmp2, cv::Mat(), cv::Point( -1, -1 ), 2/*num iteration*/ );

		cv::Mat ellipse = cv::getStructuringElement( cv::MORPH_ELLIPSE, cv::Size( 5, 5 ) );
		cv::morphologyEx( tmp2, tmp2, cv::MORPH_CLOSE, ellipse, cv::Point( -1, -1 ), 1/*num iteration*/ );

		if ( m_ShowDebugImg )
		{
			cv::imshow( "adaptiveThreshold + closing:", tmp2 );
		} // DEBUG

		std::vector< std::vector< cv::Point > > contours;
		std::vector< cv::Vec4i > hierarchy;
		cv::findContours( tmp2, contours, hierarchy, cv::RETR_CCOMP, cv::CHAIN_APPROX_SIMPLE );

		//find the contour that's greater than some sizes, i.e. the table
		double thresh = tmp2.size().width * tmp2.size().height * 0.5;

		std::vector< std::vector< cv::Point > > leftOver;
		for ( int i = 0; i < contours.size(); i++ )
		{
			double area = cv::contourArea( contours[i] );
			if ( area > thresh )
			{
				leftOver.push_back( contours[i] );
			}
		}

		if ( leftOver.size() == 1 )
		{
			tmp2 = cv::Mat::zeros( tmp2.size(), CV_8UC1 );
			drawContours( tmp2, leftOver, 0, 255/*color*/, cv::FILLED );
			// do closing to clean noise again
			cv::morphologyEx( tmp2, tmp2, cv::MORPH_CLOSE, ellipse, cv::Point( -1, -1 ), 1/*num iteration*/ );

			if ( m_ShowDebugImg )
			{
				cv::imshow( "contour:", tmp2 );
			} // DEBUG
		}
		else
		{
			// threshold failed, bail out
			tmp2 = tmp.clone();
		}

		// canny low & heigh threshold
		int low = 50;
		int high = 100;

		cv::Canny( tmp2, tmp2, low, high );

		// dilate Canny results
		cv::dilate( tmp2, tmp2, cv::Mat(), cv::Point( -1, -1 ), 2 /*num iteration*/ );

		if ( m_ShowDebugImg )
		{
			cv::imshow( "canny:", tmp2 );
		} // DEBUG

		  // mask out anything that's outside of the user-picked band
		MaskCanny( tmp2 );

		if ( m_ShowDebugImg )
		{
			cv::Mat copy = input.clone();

			// draw bounds
			cv::Scalar color = GREEN; // green

			cv::line( copy, m_o_ul, m_o_ur, color );
			cv::line( copy, m_o_ul, m_o_ll, color );
			cv::line( copy, m_o_ur, m_o_lr, color );
			cv::line( copy, m_o_ll, m_o_lr, color );

			cv::line( copy, m_i_ul, m_i_ur, color );
			cv::line( copy, m_i_ul, m_i_ll, color );
			cv::line( copy, m_i_ur, m_i_lr, color );
			cv::line( copy, m_i_ll, m_i_lr, color );

			cv::imshow( "bound:", copy );
		} // DEBUG

		  // Hough line transform
		double dRho = 1.0f;
		double dTheta = CV_PI / 180.0f;
		unsigned int minVote = 80;//360 / 2;
		float minLength = 50.0f;// 360 / 2;
		float maxGap = 10.0f;
		//LineFinder::METHOD m = LineFinder::METHOD::TRAD;
		LineFinder::METHOD m = LineFinder::METHOD::PROB;

		m_TableFinder.SetMethod( m );
		m_TableFinder.SetDeltaRho( dRho );
		m_TableFinder.SetDeltaTheta( dTheta );
		m_TableFinder.SetMinVote( minVote );
		m_TableFinder.SetMinLength( minLength );
		m_TableFinder.SetMaxGap( maxGap );

		if ( m == LineFinder::METHOD::PROB )
		{
			const std::vector<cv::Vec4i>& lines = m_TableFinder.FindLinesP( tmp2 );

			if ( m_ShowDebugImg )
			{
				//cv::Mat copy = input.clone();
				m_TableFinder.DrawDetectedLines( input, BLUE );
				cv::imshow( "HoughLine:", input );
			} // DEBUG

			// filter out the lines that's out of bound
			if ( !m_TableFinder.Refine4Edges( m_Corners, m_BandWidth, input/*debug use*/ ) )
			{
				std::cout << "error" << std::endl;
				return;
			}
		}
		else
		{
			const std::vector<cv::Vec2f>& lines = m_TableFinder.FindLines( tmp2 );
		}

		if ( m_ShowDebugImg )
		{
			m_TableFinder.DrawTableLines( input, RED );
			cv::imshow( "Filtered HoughLine:", input );
		} // DEBUG

		  // 4 corners
		TopLeft = m_TableFinder.GetTopLeft();
		TopRight = m_TableFinder.GetTopRight();
		LowerLeft = m_TableFinder.GetLowerLeft();
		LowerRight = m_TableFinder.GetLowerRight();
	}
	else
	{
		// corners is arranged by: ul, ur, ll, lr
		TopLeft = m_Corners[0];
		TopRight = m_Corners[1];
		LowerLeft = m_Corners[2];
		LowerRight = m_Corners[3];

		m_TableFinder.SetTopLeft( TopLeft );
		m_TableFinder.SetTopRight( TopRight );
		m_TableFinder.SetLowerLeft( LowerLeft );
		m_TableFinder.SetLowerRight( LowerRight );

		m_TableFinder.AvgCorners();
	} // if ( !m_ManualPickTableCorners )

	  // construct mask based on table 4 corners
	std::vector< cv::Point > tmpContour;
	tmpContour.push_back( TopRight );
	tmpContour.push_back( TopLeft );
	tmpContour.push_back( LowerLeft );
	tmpContour.push_back( LowerRight );

	std::vector< std::vector< cv::Point > > tableContour;
	tableContour.push_back( tmpContour );
	m_Mask = cv::Mat::zeros( input.size(), CV_8UC1 );
	drawContours( m_Mask, tableContour, 0, 255/*color*/, cv::FILLED );

	if ( m_ShowDebugImg )
	{
		cv::imshow( "Mask:", m_Mask );
	} // DEBUG

	  // Log table corners
	cv::Point tl( static_cast<int>( m_TableFinder.GetLeft() ), static_cast<int>( m_TableFinder.GetTop() ) );
	cv::Point tr( static_cast<int>( m_TableFinder.GetRight() ), static_cast<int>( m_TableFinder.GetTop() ) );
	cv::Point ll( static_cast<int>( m_TableFinder.GetLeft() ), static_cast<int>( m_TableFinder.GetBottom() ) );
	cv::Point lr( static_cast<int>( m_TableFinder.GetRight() ), static_cast<int>( m_TableFinder.GetBottom() ) );

	if ( m_IsLog )
	{
		m_Logger.WriteTableCorners( tl, tr, ll, lr );
	}

	m_TableFound = true;
} // FindTable

//=======================================================================
void BotManager::OnMouse( int event, int x, int y, int f, void* data )
{
	std::vector<cv::Point> *curobj = reinterpret_cast<std::vector<cv::Point>*>( data );

	if ( event == cv::EVENT_LBUTTONDOWN )
	{
		curobj->push_back( cv::Point( x, y ) );
	}
}//OnMouse

//=======================================================================
bool BotManager::SendBotMessage( const bool correctSteps )
{
	// message lay out :
	// 0, 1: Initial sync markers
	// 2, 3: desired robot pos X
	// 4, 5: desired robot pos Y
	// 6, 7: detected robot pos X
	// 8, 9: detected robot pos Y
	// 10, 11: desired X speed
    // 12, 13: desired Y speed
	BYTE message[14];

	// Initial sync markers
	message[0] = 0x7F;
	message[1] = 0x7F;

	// table size is 1003 x 597, which is within 2 bytes (2^16 = 65536, 2^8 = 256)
	// so we use 2 bytes to store position. Similarly for speed

	// desired robot pos
	const cv::Point desiredBotPos = m_Robot.GetDesiredRobotPos();

	// Pos X (high byte, low byte)
	message[2] = ( desiredBotPos.x >> 8 ) & 0xFF;
	message[3] = desiredBotPos.x & 0xFF;

	// Pos Y (high byte, low byte)
	message[4] = ( desiredBotPos.y >> 8 ) & 0xFF;
	message[5] = desiredBotPos.y & 0xFF;

	// detected robot pos
	const cv::Point detectedBotPos = correctSteps ?
		m_Camera.GetCurrBotPos() : cv::Point( -1, -1 );

	// Pos X (high byte, low byte)
	message[6] = ( detectedBotPos.x >> 8 ) & 0xFF;
	message[7] = detectedBotPos.x & 0xFF;

	// Pos Y (high byte, low byte)
	message[8] = ( detectedBotPos.y >> 8 ) & 0xFF;
	message[9] = detectedBotPos.y & 0xFF;

	// desired X speed
	const int Xspeed = m_Robot.GetDesiredRobotXSpeed();
	message[10] = ( Xspeed >> 8 ) & 0xFF;
	message[11] = Xspeed & 0xFF;

    // desired Y speed
    const int Yspeed = m_Robot.GetDesiredRobotYSpeed();
    message[12] = ( Yspeed >> 8 ) & 0xFF;
    message[13] = Yspeed & 0xFF;

	return m_pSerialPort->WriteSerialPort<BYTE>( message, 14 );
} // SendBotMessage

//=======================================================================
void BotManager::ReceiveMessage()
{
    char msg[200];

    int res = m_pSerialPort->ReadSerialPort<char>( msg, 200 );
    std::cout << msg << std::endl;

} // ReceiveMessage

//=======================================================================
void BotManager::OrderCorners()
{
	if (m_Corners.size() != 4)
	{
		return;
	}

	cv::Point left1(100000, 0); // left most
	cv::Point left2(100000, 0); // 2nd to left most
	cv::Point right1(-1, 0); // right most
	cv::Point right2(-1, 0); // 2nd to right most

	for (int i = 0; i < 4; i++)
	{
		if (m_Corners[i].x < left1.x)
		{
			left2 = left1;
			left1 = m_Corners[i];
		}
		else if (m_Corners[i].x < left2.x)
		{
			left2 = m_Corners[i];
		}

		if (m_Corners[i].x > right1.x)
		{
			right2 = right1;
			right1 = m_Corners[i];
		}
		else if (m_Corners[i].x > right2.x)
		{
			right2 = m_Corners[i];
		}
	} // for i = 1 - 4

	// determine upper and lower
	// m_Corners is arranged by: ul, ur, ll, lr

	if (left1.y > left2.y)
	{
		// left1 is lower left, left2 is upper left
		m_Corners[0] = left2;
		m_Corners[2] = left1;
	}
	else
	{
		// left2 is lower left, left1 is upper left
		m_Corners[0] = left1;
		m_Corners[2] = left2;
	}

	if (right1.y > right2.y)
	{
		// right1 is lower right, right2 is upper right
		m_Corners[1] = right2;
		m_Corners[3] = right1;
	}
	else
	{
		// right2 is lower right, right1 is upper right
		m_Corners[1] = right1;
		m_Corners[3] = right2;
	}

}// OrderCorners

//=======================================================================
void BotManager::MaskCanny(cv::Mat & img)
{
    // Generate a band around user-picked 4 corners.
    // This band is wider than the m_BandWidth because
    // such that later in Hough transform, it doesn't
    // detect the artificial line generated by the masking.
    // After Hough transform line detection, we then
    // mask again using the true m_BandWidth
    cv::Size s = img.size();

    float o_l, o_r, o_t, o_b;
    float i_l, i_r, i_t, i_b;

    const unsigned int offset = m_BandWidth + 10;

    Utility::GenerateBand(
        o_l, o_r, o_t, o_b,
        i_l, i_r, i_t, i_b,
        m_o_ul, m_o_ur, m_o_ll, m_o_lr,
        m_i_ul, m_i_ur, m_i_ll, m_i_lr,
        m_Corners, offset, offset );

	for (unsigned int i = 0; i < static_cast<unsigned int>(s.width); i++)
	{
		for (unsigned int j = 0; j < static_cast<unsigned int>(s.height); j++)
		{
			const bool isOutsideOuter = Utility::IsOutsideOuter( i, j, o_l, o_r, o_t, o_b, m_o_ur, m_o_ll, m_o_lr );
			if ( isOutsideOuter )
			{
				img.at<uchar>(j, i) = 0;
			}
			else
			{
				const bool isInsideInner = Utility::IsInsideInner(i, j, i_l, i_r, i_t, i_b, m_i_ur, m_i_ll, m_i_lr );
				if ( isInsideInner )
				{
					img.at<uchar>(j, i) = 0;
				}
			}
		}
	}

	if ( m_ShowDebugImg )
	{
		cv::imshow( "masked canny:", img );
	} // DEBUG

}//void MaskCanny(cv::Mat & img);

//=======================================================================
void BotManager::SetShowDebugImg( const bool ok )
{
	m_ShowDebugImg = ok;
} // SetShowDebugImg

//=======================================================================
void BotManager::SetManualPickTableCorners( const bool ok )
{
	m_ManualPickTableCorners = ok;
} // SetManualPickTableCorners

//=======================================================================
void BotManager::SetShowOutPutImg( const bool ok )
{
	m_ShowOutPutImg = ok;
} // SetShowOutPutImg