/*************************************************************************
 * Copyright (c) 2014 eProsima. All rights reserved.
 *
 * This copy of eProsima RTPS is licensed to you under the terms described in the
 * EPROSIMARTPS_LIBRARY_LICENSE file included in this distribution.
 *
 *************************************************************************/

/**
 * @file RTPSMessageGroup.cpp
 *
 */

#include "eprosimartps/writer/RTPSMessageGroup.h"
#include "eprosimartps/RTPSMessageCreator.h"
#include "eprosimartps/writer/RTPSWriter.h"
#include "eprosimartps/resources/ResourceSend.h"

#include "eprosimartps/utils/RTPSLog.h"

namespace eprosima {
namespace rtps {

bool sort_changes_group (CacheChange_t* c1,CacheChange_t* c2)
{
	return(c1->sequenceNumber.to64long() < c2->sequenceNumber.to64long());
}

bool sort_SeqNum(SequenceNumber_t s1,SequenceNumber_t s2)
{
	return(s1.to64long() < s2.to64long());
}

typedef std::pair<SequenceNumber_t,SequenceNumberSet_t> pair_T;

void RTPSMessageGroup::prepare_SequenceNumberSet(std::vector<SequenceNumber_t>* changesSeqNum,
		std::vector<std::pair<SequenceNumber_t,SequenceNumberSet_t>>* sequences)
{
	//First compute the number of GAP messages we need:

	std::sort(changesSeqNum->begin(),changesSeqNum->end(),sort_SeqNum);
	bool new_pair = true;
	bool seqnumset_init = false;
	uint32_t count = 0;
	for(std::vector<SequenceNumber_t>::iterator it = changesSeqNum->begin();
			it!=changesSeqNum->end();++it)
	{
		if(new_pair)
		{
			SequenceNumberSet_t seqset;
			seqset.base = (*it)+1; // IN CASE IN THIS SEQNUMSET there is only 1 number.
			pair_T pair(*it,seqset);
			sequences->push_back(pair);
			new_pair = false;
			seqnumset_init = false;
			count = 1;
			continue;
		}
		if((*it).to64long() - sequences->back().first.to64long() == count) //CONTINUOUS FROM THE START
		{
			++count;
			sequences->back().second.base = (*it)+1;
			continue;
		}
		else
		{
			if(!seqnumset_init) //FIRST TIME SINCE it was continuous
			{
				sequences->back().second.base = *(it-1);
				seqnumset_init = false;
			}
			if(sequences->back().second.add((*it)))
				continue;
			else
			{
				--it;
				new_pair = true;
			}
		}
	}
}







void RTPSMessageGroup::send_Changes_AsGap(RTPSMessageGroup_t* msg_group,
		RTPSWriter* W,
		std::vector<SequenceNumber_t>* changesSeqNum, const EntityId_t& readerId,
		LocatorList_t* unicast, LocatorList_t* multicast)
{
	//cout << "CHANGES SEQ NUM: "<<changesSeqNum->size()<<endl;
	std::vector<std::pair<SequenceNumber_t,SequenceNumberSet_t>> Sequences;
	RTPSMessageGroup::prepare_SequenceNumberSet(changesSeqNum,&Sequences);
	std::vector<std::pair<SequenceNumber_t,SequenceNumberSet_t>>::iterator seqit = Sequences.begin();

	CDRMessage_t* cdrmsg_submessage = &msg_group->m_rtpsmsg_submessage;
	CDRMessage_t* cdrmsg_header = &msg_group->m_rtpsmsg_header;
	CDRMessage_t* cdrmsg_fullmsg = &msg_group->m_rtpsmsg_fullmsg;

	uint16_t gap_msg_size = 0;
	uint16_t gap_n = 1;
	//FIRST SUBMESSAGE
	CDRMessage::initCDRMsg(cdrmsg_submessage);
	RTPSMessageCreator::addSubmessageGap(cdrmsg_submessage,seqit->first,seqit->second,
			readerId,W->getGuid().entityId);

	gap_msg_size = cdrmsg_submessage->length;
	if(gap_msg_size+RTPSMESSAGE_HEADER_SIZE > RTPSMESSAGE_MAX_SIZE)
	{
		pError("The Gap messages are larger than max size, fragmentation needed" << endl);
	}
	bool first = true;
	do
	{
		CDRMessage::initCDRMsg(cdrmsg_fullmsg);
		CDRMessage::appendMsg(cdrmsg_fullmsg,cdrmsg_header);
		if(first)
		{
			CDRMessage::appendMsg(cdrmsg_fullmsg,cdrmsg_submessage);
			first = false;
		}
		while(cdrmsg_fullmsg->length + gap_msg_size < cdrmsg_fullmsg->max_size
				&& (gap_n + 1) <=(uint16_t)Sequences.size()) //another one fits in the full message
		{
			++gap_n;
			++seqit;
			CDRMessage::initCDRMsg(cdrmsg_submessage);
			RTPSMessageCreator::addSubmessageGap(cdrmsg_submessage,seqit->first,seqit->second,
					readerId,W->getGuid().entityId);
			CDRMessage::appendMsg(cdrmsg_fullmsg,cdrmsg_fullmsg);
		}
		std::vector<Locator_t>::iterator lit;
		for(lit = unicast->begin();lit!=unicast->end();++lit)
			W->mp_send_thr->sendSync(cdrmsg_fullmsg,(*lit));
		for(lit = multicast->begin();lit!=multicast->end();++lit)
			W->mp_send_thr->sendSync(cdrmsg_fullmsg,(*lit));

	}while(gap_n < Sequences.size()); //There is still a message to add
}

void RTPSMessageGroup::prepareDataSubM(RTPSWriter* W,CDRMessage_t* submsg,bool expectsInlineQos,CacheChange_t* change,const EntityId_t& ReaderId)
{
	ParameterList_t* inlineQos = NULL;
	if(expectsInlineQos)
	{
		if(W->getInlineQos()->m_parameters.size()>0)
			inlineQos = W->getInlineQos();
	}
	CDRMessage::initCDRMsg(submsg);
	RTPSMessageCreator::addSubmessageData(submsg,change,W->getTopic().getTopicKind(),ReaderId,expectsInlineQos,inlineQos);
}


void RTPSMessageGroup::send_Changes_AsData(RTPSMessageGroup_t* msg_group,
		RTPSWriter* W,
		std::vector<CacheChange_t*>* changes,
		LocatorList_t& unicast, LocatorList_t& multicast,
		bool expectsInlineQos,const EntityId_t& ReaderId)
{
	pDebugInfo("Sending relevant changes as data messages" << endl);
	CDRMessage_t* cdrmsg_submessage = &msg_group->m_rtpsmsg_submessage;
	CDRMessage_t* cdrmsg_header = &msg_group->m_rtpsmsg_header;
	CDRMessage_t* cdrmsg_fullmsg = &msg_group->m_rtpsmsg_fullmsg;

	std::vector<CacheChange_t*>::iterator cit =changes->begin();

	uint16_t data_msg_size = 0;
	uint16_t change_n = 1;
	//FIRST SUBMESSAGE
	RTPSMessageGroup::prepareDataSubM(W,cdrmsg_submessage, expectsInlineQos,*cit,ReaderId);
	data_msg_size = cdrmsg_submessage->length;
	if(data_msg_size+RTPSMESSAGE_HEADER_SIZE > RTPSMESSAGE_MAX_SIZE)
	{
		pError("The Data messages are larger than max size, fragmentation needed" << endl);
	}
	bool first = true;
	do
	{
		CDRMessage::initCDRMsg(cdrmsg_fullmsg);
		CDRMessage::appendMsg(cdrmsg_fullmsg,cdrmsg_header);
		RTPSMessageCreator::addSubmessageInfoTS_Now(cdrmsg_fullmsg,false);
		if(first)
		{
			CDRMessage::appendMsg(cdrmsg_fullmsg,cdrmsg_submessage);
			first = false;
		}
		while(cdrmsg_fullmsg->length + data_msg_size < cdrmsg_fullmsg->max_size
				&& (change_n + 1) <= (uint16_t)changes->size()) //another one fits in the full message
		{
			++change_n;
			++cit;
			RTPSMessageGroup::prepareDataSubM(W,cdrmsg_submessage, expectsInlineQos,*cit,ReaderId);
			CDRMessage::appendMsg(cdrmsg_fullmsg,cdrmsg_submessage);
		}
		for(std::vector<Locator_t>::iterator lit = unicast.begin();lit!=unicast.end();++lit)
			W->mp_send_thr->sendSync(cdrmsg_fullmsg,(*lit));


		for(std::vector<Locator_t>::iterator lit = multicast.begin();lit!=multicast.end();++lit)
			W->mp_send_thr->sendSync(cdrmsg_fullmsg,(*lit));


	}while(change_n < changes->size()); //There is still a message to add
}

void RTPSMessageGroup::send_Changes_AsData(RTPSMessageGroup_t* msg_group,
		RTPSWriter* W,
		std::vector<CacheChange_t*>* changes,const Locator_t& loc,
		bool expectsInlineQos,const EntityId_t& ReaderId)
{
	pDebugInfo("Sending relevant changes as data messages" << endl);
	CDRMessage_t* cdrmsg_submessage = &msg_group->m_rtpsmsg_submessage;
	CDRMessage_t* cdrmsg_header = &msg_group->m_rtpsmsg_header;
	CDRMessage_t* cdrmsg_fullmsg = &msg_group->m_rtpsmsg_fullmsg;
	uint16_t data_msg_size = 0;
	uint16_t change_n = 1;
	//FIRST SUBMESSAGE
	std::vector<CacheChange_t*>::iterator cit = changes->begin();
	RTPSMessageGroup::prepareDataSubM(W,cdrmsg_submessage, expectsInlineQos,*cit,ReaderId);
	data_msg_size = cdrmsg_submessage->length;
	if(data_msg_size+RTPSMESSAGE_HEADER_SIZE > RTPSMESSAGE_MAX_SIZE)
	{
		pError("The Data messages are larger than max size, fragmentation needed" << endl);
	}
	bool first = true;
	do
	{
		CDRMessage::initCDRMsg(cdrmsg_fullmsg);
		CDRMessage::appendMsg(cdrmsg_fullmsg,cdrmsg_header);
		RTPSMessageCreator::addSubmessageInfoTS_Now(cdrmsg_fullmsg,false);
		if(first)
		{
			CDRMessage::appendMsg(cdrmsg_fullmsg,cdrmsg_submessage);
			first = false;
		}
		while(cdrmsg_fullmsg->length + data_msg_size < cdrmsg_fullmsg->max_size
				&& (change_n + 1) <= (uint16_t)changes->size()) //another one fits in the full message
		{
			++change_n;
			++cit;
			RTPSMessageGroup::prepareDataSubM(W,cdrmsg_submessage, expectsInlineQos,*cit,ReaderId);
			CDRMessage::appendMsg(cdrmsg_fullmsg,cdrmsg_submessage);
		}
		W->mp_send_thr->sendSync(cdrmsg_fullmsg,loc);
	}while(change_n < changes->size()); //There is still a message to add
}














} /* namespace rtps */
} /* namespace eprosima */
