//-----------------------------------------------------------------------------
//
//	ApplicationStatus.cpp
//
//	Implementation of the Z-Wave COMMAND_CLASS_APPLICATION_STATUS
//
//	Copyright (c) 2010 Mal Lansell <openzwave@lansell.org>
//
//	SOFTWARE NOTICE AND LICENSE
//
//	This file is part of OpenZWave.
//
//	OpenZWave is free software: you can redistribute it and/or modify
//	it under the terms of the GNU Lesser General Public License as published
//	by the Free Software Foundation, either version 3 of the License,
//	or (at your option) any later version.
//
//	OpenZWave is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//	GNU Lesser General Public License for more details.
//
//	You should have received a copy of the GNU Lesser General Public License
//	along with OpenZWave.  If not, see <http://www.gnu.org/licenses/>.
//
//-----------------------------------------------------------------------------

#include "command_classes/CommandClasses.h"
#include "command_classes/ApplicationStatus.h"
#include "Defs.h"
#include "Msg.h"
#include "Driver.h"
#include "Notification.h"
#include "TimerThread.h"
#include "platform/Log.h"

namespace OpenZWave
{
	namespace Internal
	{
		namespace CC
		{

			enum ApplicationStatusCmd
			{
				ApplicationStatusCmd_Busy = 0x01,
				ApplicationStatusCmd_RejectedRequest = 0x02
			};

//-----------------------------------------------------------------------------
// <ApplicationStatus::ApplicationStatus>
// Constructor
//-----------------------------------------------------------------------------
			ApplicationStatus::ApplicationStatus(uint32 const _homeId, uint8 const _nodeId) :
					CommandClass(_homeId, _nodeId), m_mutex(new Internal::Platform::Mutex()), m_busy(false)
			{
				Timer::SetDriver(GetDriver());
			}

//-----------------------------------------------------------------------------
// <ApplicationStatus::~ApplicationStatus>
// Destructor
//-----------------------------------------------------------------------------
			ApplicationStatus::~ApplicationStatus()
			{
				m_mutex->Release();
				while (!m_busyQueue.empty())
				{
					Driver::MsgQueueItem const& item = m_busyQueue.front();
					if (Driver::MsgQueueCmd_SendMsg == item.m_command)
					{
						delete item.m_msg;
					}
					else if (Driver::MsgQueueCmd_Controller == item.m_command)
					{
						delete item.m_cci;
					}
					m_busyQueue.pop_front();
				}
			}

//-----------------------------------------------------------------------------
// <ApplicationStatus::HandleMsg>
// Handle a message from the Z-Wave network
//-----------------------------------------------------------------------------
			bool ApplicationStatus::HandleMsg(uint8 const* _data, uint32 const _length, uint32 const _instance	// = 1
					)
			{
				Notification* notification = new Notification(Notification::Type_UserAlerts);
				notification->SetHomeAndNodeIds(GetHomeId(), GetNodeId());
				if (ApplicationStatusCmd_Busy == (ApplicationStatusCmd) _data[0])
				{
					switch (_data[1])
					{
						case 0:
						{
							// Mark as busy for 1000 ms
							SetBusy(1000);
							notification->SetUserAlertNotification(Notification::Alert_ApplicationStatus_Retry);
							break;
						}
						case 1:
						{
							// Mark as busy for _data[2] seconds
							SetBusy(1000*_data[2]);
							notification->SetUserAlertNotification(Notification::Alert_ApplicationStatus_Retry);
							notification->SetRetry(_data[2]);
							break;
						}
						case 2:
						{
							notification->SetUserAlertNotification(Notification::Alert_ApplicationStatus_Queued);
							break;
						}
						default:
						{
							// Invalid status
							Log::Write(LogLevel_Warning, GetNodeId(), "Received a unknown Application Status Message %d - Assuming Rejected", _data[1]);
							notification->SetUserAlertNotification(Notification::Alert_ApplicationStatus_Rejected);
						}
					}
				}

				if (ApplicationStatusCmd_RejectedRequest == (ApplicationStatusCmd) _data[0])
				{
					notification->SetUserAlertNotification(Notification::Alert_ApplicationStatus_Rejected);
				}

				GetDriver()->QueueNotification(notification);

				return true;
			}

			//-----------------------------------------------------------------------------
			// <ApplicationStatus::SetBusy>
			// Set busy flag for _ms_duration milliseconds
			// Move pending messages to busy queue
			// Add timer to clear busy flag
			//-----------------------------------------------------------------------------
			void ApplicationStatus::SetBusy(int32 _ms_duration)
			{
				if (m_busy)
				{
					// The node is already busy...
					if (m_busyTimeStamp.TimeRemaining() > _ms_duration) {
						// ...and the running timer is longer. Nothing to do.
						return;
					} else {
						// ... and the running timer is shorter. Delete and recreate below.
						TimerDelEvents();
					}
				} else {
					// Flag node as busy
					m_busy = true;
					// Move queued messages to busy queue
					GetDriver()->MoveMessagesToBusyQueue(GetNodeId());
				}

				// Save end timestamp for this duration
				m_busyTimeStamp.SetTime(_ms_duration);

				// Add timer to mark node unbusy
				Internal::TimerThread::TimerCallback callback = bind(&ApplicationStatus::ClearBusy, this);
				TimerSetEvent(_ms_duration, callback, 1);
			}

			void ApplicationStatus::ClearBusy()
			{
				// Mark node as not busy
				m_busy = false;

				// Re-queue pending messages
				// Copied from WakeUp::SendPending()
				m_mutex->Lock();
				list<Driver::MsgQueueItem>::iterator it = m_busyQueue.begin();
				while (it != m_busyQueue.end())
				{
					Driver::MsgQueueItem const& item = *it;
					if (Driver::MsgQueueCmd_SendMsg == item.m_command)
					{
						GetDriver()->SendMsg(item.m_msg, Driver::MsgQueue_WakeUp);
					}
					else if (Driver::MsgQueueCmd_QueryStageComplete == item.m_command)
					{
						GetDriver()->SendQueryStageComplete(item.m_nodeId, item.m_queryStage);
					}
					else if (Driver::MsgQueueCmd_Controller == item.m_command)
					{
						GetDriver()->BeginControllerCommand(item.m_cci->m_controllerCommand, item.m_cci->m_controllerCallback, item.m_cci->m_controllerCallbackContext, item.m_cci->m_highPower, item.m_cci->m_controllerCommandNode, item.m_cci->m_controllerCommandArg);
						delete item.m_cci;
					}
					else if (Driver::MsgQueueCmd_ReloadNode == item.m_command)
					{
						GetDriver()->ReloadNode(item.m_nodeId);
					}
					it = m_busyQueue.erase(it);
				}
				m_mutex->Unlock();
			}

			//-----------------------------------------------------------------------------
			// <ApplicationStatus::QueueMsg>
			// Add a Z-Wave message to the busy queue
			//-----------------------------------------------------------------------------
			void ApplicationStatus::QueueMsg(Driver::MsgQueueItem const& _item)
			{
				m_mutex->Lock();
				/* make sure the SendAttempts is reset to 0 */
				// FIXME: Is this needed?
				if (_item.m_command == Driver::MsgQueueCmd_SendMsg)
					_item.m_msg->SetSendAttempts(0);

				m_msgQueue.push_back(_item);
				m_mutex->Unlock();
			}

		} // namespace CC
	} // namespace Internal
} // namespace OpenZWave

