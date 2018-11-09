//////////////////////////////////////////////////////
// Original Author: Jose Julio
// Modified by Alex Chen
//////////////////////////////////////////////////////
#include "Robot.h"
#include "Configuration.h"
#include <iostream>

//====================================================================================================================
Robot::Robot()
	: m_RobotStatus( BOT_STATUS::INIT )
	, m_AttackTime( 0 )
	, m_AttackStatus( ATTACK_STATUS::WAIT_FOR_ATTACK )
    , m_DesiredSpeed( 0 )
{}

//====================================================================================================================
Robot::~Robot()
{}

//====================================================================================================================
int Robot::GetDesiredRobotSpeed()
{
	return m_DesiredSpeed;
}

//====================================================================================================================
cv::Point Robot::GetDesiredRobotPos()
{
    return m_DesiredRobotPos;
} // GetDesiredRobotPos

//====================================================================================================================
void Robot::NewDataStrategy( Camera& cam )
{
	m_RobotStatus = BOT_STATUS::INIT; // Going to initial position (defense)

    switch( cam.GetPredictStatus() )
    {
        case Camera::PREDICT_STATUS::NO_RISK:
            {
                // If the puck is moving slowly in the robot field we could start an attack
                if( cam.GetCurrPuckPos().y < ROBOT_CENTER_Y - 20 &&
                    std::abs( cam.GetCurrPuckSpeed().y ) < MIN_ABS_Y_SPEED &&
                    std::abs( cam.GetCurrPuckSpeed().x ) < MIN_ABS_X_SPEED )
                {
                    m_RobotStatus = BOT_STATUS::ATTACK;
                }
            }
            break;

        case Camera::PREDICT_STATUS::DIRECT_IMPACT:
            {
				const unsigned int prevNumPredictBounce = cam.GetPrevNumPredictBounce();

				if ( prevNumPredictBounce == 0 )
				{
					// Puck is moving to our field directly, with no bounce; Direct impact
					cv::Point predictPos = cam.GetCurrPredictPos();

					if ( ( predictPos.x > ROBOT_MIN_X + PUCK_SIZE * 2 ) &&
						( predictPos.x < ROBOT_MAX_X - PUCK_SIZE * 2 ) )
					{
						// Predicted position X is within table range
						if ( cam.GetPuckAvgSpeed().y > MIN_PUCK_Y_SPEED1 )
						{
							// puck is moving not so fast
							m_RobotStatus = BOT_STATUS::DEFENCE_AND_ATTACK;
						}
						else
						{
							// puck is moving fast
                            m_RobotStatus = BOT_STATUS::DEFENCE;
						}
					}
					else
					{
						// Predicted position X is out of table range
						if ( cam.GetPredictTimeDefence() < BOT_MOVE_TIME_THRESHOLD )
						{
							// predicted hit time is small, i.e. puck approcahing soon
							m_RobotStatus = BOT_STATUS::DEFENCE;
						}
						else
						{
							// predicted hit time is large, i.e. no risk
							m_RobotStatus = BOT_STATUS::INIT;
						}
					}
				}
				else if ( prevNumPredictBounce == 1 )
				{
					// puck coming from a bounce, this is the first frame after the bounce
					if ( cam.GetPuckAvgSpeed().y > MIN_PUCK_Y_SPEED2 )
					{
						// puck is moving not so fast
						m_RobotStatus = BOT_STATUS::DEFENCE_AND_ATTACK;
					}
					else
					{
						// puck is moving fast
						m_RobotStatus = BOT_STATUS::DEFENCE;
					}

				}
            }
            break;

        case Camera::PREDICT_STATUS::ONE_BOUNCE:
            {
                // Prediction with side bounce
                if( cam.GetPredictTimeDefence() < BOT_MOVE_TIME_THRESHOLD )
                {
                    // Limit movement
                    m_RobotStatus = BOT_STATUS::DEFENCE;
                }
                else
                {
                    m_RobotStatus = BOT_STATUS::INIT;
                }
            }
            break;
        default:
            break;
    } // switch

} // Robot::NewDataStrategy

//====================================================================================================================
void Robot::RobotMoveDecision( Camera& cam )
{
    m_DesiredSpeed = MAX_ABS_SPEED;

    cv::Point robotPos;

	switch ( m_RobotStatus )
	{
	case BOT_STATUS::INIT: // Go to init position
	{
		if ( !IsOwnGoal( cam ) )
        {
            m_DesiredRobotPos.x = ROBOT_CENTER_X;  //center X axis
            m_DesiredRobotPos.y = ROBOT_DEFENSE_POSITION_DEFAULT;
            m_DesiredSpeed = static_cast<int>( MAX_ABS_SPEED * 0.6667 ); // Return a bit more slowly...
        }

		m_AttackTime = 0;
	}
	break;

	case BOT_STATUS::DEFENCE: // Defense mode (only move on X axis on the defense line)
	{
		cv::Point pos = cam.GetCurrPredictPos();
		const int maxX = TABLE_WIDTH - PUCK_SIZE * 3;
		const int minX = PUCK_SIZE * 3;

		if ( pos.x < minX )
		{
			pos.x = minX;
		}
		else if ( pos.x > maxX )
		{
			pos.x = maxX;
		}

		cam.SetCurrPredictPos( pos );

		m_DesiredRobotPos.y = ROBOT_DEFENSE_POSITION_DEFAULT;
        m_DesiredRobotPos.x = pos.x;

		m_AttackTime = 0;
	} // case 1
	break;

	case BOT_STATUS::DEFENCE_AND_ATTACK:
	{
		if ( cam.GetPredictTimeAttack() < MIN_PREDICT_TIME ) // If time is less than 150ms we start the attack
		{
            m_DesiredRobotPos.y = ROBOT_DEFENSE_ATTACK_POSITION_DEFAULT + PUCK_SIZE * 4; // we need some override
            m_DesiredRobotPos.x = cam.GetPredictXAttack();
		}
		else      // Defense position
		{
            m_DesiredRobotPos.y = ROBOT_DEFENSE_POSITION_DEFAULT;
            m_DesiredRobotPos.x = cam.GetCurrPredictPos().x;  // predict_x_attack;

			m_AttackTime = 0;
		}
	} // case 2
	break;

	case BOT_STATUS::ATTACK:
	{
		cv::Point attackPredictPos;

		if ( m_AttackTime == 0 )
		{
            if( !IsOwnGoal( cam ) )
            {
                attackPredictPos = cam.PredictPuckPos( ATTACK_TIME_THRESHOLD );

                if( ( attackPredictPos.x > PUCK_SIZE * 3 ) &&
                    ( attackPredictPos.x < TABLE_WIDTH - PUCK_SIZE * 3 ) &&
                    ( attackPredictPos.y > PUCK_SIZE * 4 ) &&
                    ( attackPredictPos.y < ROBOT_CENTER_Y - PUCK_SIZE * 5 ) )
                {
                    m_AttackTime = clock() + static_cast<clock_t>( ATTACK_TIME_THRESHOLD * CLOCKS_PER_SEC / 1000.0f );  // Prepare an attack in 500ms

                    // Go to pre-attack position
                    m_DesiredRobotPos.x = attackPredictPos.x;
                    m_DesiredRobotPos.y = attackPredictPos.y - PUCK_SIZE * 4;

                    m_DesiredSpeed = static_cast<int>( MAX_ABS_SPEED * 0.5 );

                    m_AttackStatus = ATTACK_STATUS::READY_TO_ATTACK;
                }
                else
                {
                    m_AttackTime = 0;  // Continue waiting for the right attack moment...
					m_AttackStatus = ATTACK_STATUS::WAIT_FOR_ATTACK;

                    // And go to defense position
                    m_DesiredRobotPos.y = ROBOT_DEFENSE_POSITION_DEFAULT;
                    m_DesiredRobotPos.x = ROBOT_CENTER_X;  //center X axis
                    m_DesiredSpeed = static_cast<int>( MAX_ABS_SPEED * 0.6667 ); // Return a bit more slowly...
                }
            } // if( !IsOwnGoal( cam ) )
		} // if ( m_AttackTime == 0 )
		else
		{
			if ( m_AttackStatus == ATTACK_STATUS::READY_TO_ATTACK )
			{
				// ready to attack
                if( !IsOwnGoal( cam ) )
                {
                    const int impactTime = static_cast<int>( ( m_AttackTime - clock() ) * 1000.0f / CLOCKS_PER_SEC ); // in ms
                    if( impactTime < IMPACT_TIME_THRESHOLD )  // less than 150ms to start the attack
                    {
                        // Attack movement
                        attackPredictPos = cam.PredictPuckPos( impactTime );
                        m_DesiredRobotPos.x = attackPredictPos.x;
                        m_DesiredRobotPos.y = attackPredictPos.y + PUCK_SIZE * 2;

                        m_AttackStatus = ATTACK_STATUS::AFTER_ATTACK;
                    }
                    else  // m_AttackStatus = ATTACK_STATUS::READY_TO_ATTACK but it's not the time to attack yet
                    {
                        // Go to pre-attack position
                        attackPredictPos = cam.PredictPuckPos( ATTACK_TIME_THRESHOLD );

                        m_DesiredRobotPos.x = attackPredictPos.x;
                        m_DesiredRobotPos.y = attackPredictPos.y - PUCK_SIZE * 4;

                        m_DesiredSpeed = static_cast<int>( MAX_ABS_SPEED * 0.5 );
                    }
                }//if( !IsOwnGoal( cam ) )
			} // if (m_AttackStatus == ATTACK_STATUS::READY_TO_ATTACK)

			if ( m_AttackStatus == ATTACK_STATUS::AFTER_ATTACK )
			{
				// after firing attack
				int dt = static_cast<int>( ( clock() - m_AttackTime ) * 1000.0f / CLOCKS_PER_SEC ); // in ms

				if ( dt > 80 ) // Attack move is done? => Reset to defense position
				{
					//Serial.print( "RESET" );
					m_AttackTime = 0;
					m_RobotStatus = BOT_STATUS::INIT;
					m_AttackStatus = ATTACK_STATUS::WAIT_FOR_ATTACK;
				}
			} // if ( m_AttackStatus == ATTACK_STATUS::AFTER_ATTACK )
		} // if (m_AttackTime == 0)
	} // case 3
	break;
	case 4: // The puck came from a bounce
	{
		// Only defense now (we could improve this in future)
		// Defense mode (only move on X axis on the defense line)
		cv::Point pos = cam.GetCurrPredictPos();

		const int minX = PUCK_SIZE * 3;
		const int maxX = TABLE_WIDTH - PUCK_SIZE * 3;

		// we leave some space near the borders...
		if ( pos.x < minX )
		{
			pos.x = minX;
		}
		else if ( pos.x > maxX )
		{
			pos.x = maxX;
		}

		cam.SetCurrPredictPos( pos );

        m_DesiredRobotPos.y = ROBOT_DEFENSE_POSITION_DEFAULT;
        m_DesiredRobotPos.x = pos.x;

		m_AttackTime = 0;
	}
	break;

	case 5:
	{
		// User manual control
		//hBot.SetMaxAbsSpeed( m_UserSpeed );
		// Control acceleration
		//hBot.SetMaxAbsAccel( m_UserAccel );
		//hBot.SetPosStraight( m_UserPuckPos.x, m_UserPuckPos.y );
		//Serial.println(max_acceleration);
	}// case 5
	break;

	default:
		// Default : go to defense position
		if ( !IsOwnGoal( cam ) )
		{
            m_DesiredRobotPos.y = ROBOT_DEFENSE_POSITION_DEFAULT;
            m_DesiredRobotPos.x = ROBOT_CENTER_X;  //center X axis
		}

		m_AttackTime = 0;
	}// switch
} // Robot::RobotMoveDecision

//====================================================================================================================
bool Robot::IsOwnGoal( const Camera& cam )
{
	const int rotPosY  = cam.GetRobotPos().y;
	const int puckPosX = cam.GetCurrPuckPos().x;
	const int puckPosY = cam.GetCurrPuckPos().y;

	if ( rotPosY  > ROBOT_DEFENSE_POSITION_DEFAULT + PUCK_SIZE  &&  // robot in front of defence line
    	 puckPosY < rotPosY && // puck behind robot
		 puckPosX > ROBOT_CENTER_X - PUCK_SIZE * 5 &&     // puck X within table range
		 puckPosX < ROBOT_CENTER_X + PUCK_SIZE * 5 )
	{
		return true;
	}
	else
	{
		return false;
	}
} // IsOwnGoal
