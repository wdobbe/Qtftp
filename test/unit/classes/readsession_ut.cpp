/****************************************************************************
* Copyright (c) Contributors as noted in the AUTHORS file
*
* This file is part of QTFTP.
*
* QTFTP is free software; you can redistribute it and/or modify it under
* the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2.1 of the License, or
* (at your option) any later version.
*
* QTFTP is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
****************************************************************************/

#if defined _MSC_VER
//Unbelievable, windows.h defines a macro min that messes up the std::min standard c++ library .....
#define NOMINMAX
#endif
#include "qtftp/readsession.h"
#include "udpsocketstubfactory.h"
#include "simulatednetworkstream.h"
//#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QByteArray>
#include <QTest>
#include <algorithm>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
#include <thread>
#include <iostream>

namespace QTFTP
{


class ReadSessionTest : public QObject
{
    Q_OBJECT

    public:

        ReadSessionTest() : m_socketFactory(std::make_shared<UdpSocketStubFactory>())
        {
            if (m_rrqOpcode == 0x0)
            {
                m_rrqOpcode = htons(0x0001);
                m_ackOpcode = htons(0x0004);
            }
        }

        QByteArray createReadSessionAndReturnNetworkResponse(const QHostAddress &peerAddr, uint16_t peerPort, const QByteArray &rrqDatagram)
        {
            m_readSession  = std::make_unique<ReadSession>(peerAddr, peerPort, rrqDatagram, TFTP_TEST_FILES_DIR, 2000, m_socketFactory);
            //the source port of the session socket is randomly choosen, but there should be only 1 socket, so find any
            SimulatedNetworkStream &outNetworkStream = m_socketFactory->getNetworkStreamBySource(UdpSocketStubFactory::StreamDirection::Output, QHostAddress::Any, 0);
            QByteArray sentData;
            outNetworkStream >> sentData;
            return sentData;
        }

        std::shared_ptr<UdpSocketStubFactory> m_socketFactory; //we need to hold on to this to retrieve sockets later

        static uint16_t m_rrqOpcode;
        static uint16_t m_ackOpcode;
        std::unique_ptr<ReadSession> m_readSession;

    private slots:
        void errorOnMailTransferMode();
        void errorOnIllegalTransferMode();
        void errorOnNonExistingFile();
        void errorOnNonReadableFile();
        void transferFileSmallerThanOneBlockBinary();
        void transferFileLargerThanOneBlockBinary();
        void transferFileExactMultipleOfBlockSizeBinary();
        void retransmitDataBlockOnTimeout();
        void retransmitMultiplePackets();
        void transmitFileSmallerThanOneBlockAscii();
        void transmitFileLargerThanOneBlockAscii();
        void detectSlowNetwork();
        void transmitOackOnOptionsRrq();

};


uint16_t ReadSessionTest::m_rrqOpcode( 0x0 ); //not allowed to call htons function at file scope
uint16_t ReadSessionTest::m_ackOpcode( 0x0 );

static qint64 readBytesFromFile(QByteArray &destination, const QString &fileName, qint64 offsetInFile, qint64 nrOfBytes)
{
    QFile testFile( QString(TFTP_TEST_FILES_DIR) + '/' + fileName );
    bool openResult = testFile.open(QIODevice::ReadOnly);
    if ( ! openResult )
    {
        return -1;
    }
    testFile.seek(offsetInFile);
    destination.resize( static_cast<int>(std::min(nrOfBytes, testFile.bytesAvailable())) );
    auto bytesRead = testFile.read( destination.data(), destination.size() );
    return bytesRead;
}


//TODO: test transfer with overflow of blocknr
//TODO: test what happens if client sends ACK with wrong blocknr, currently this results in crash

/**
 * @brief ReadSessionTest::errorOnMailTransferMode
 *
 * QTFTP doesn't support transfermode 'Mail', so make sure the appropriate error is sent if we receive a request that uses it
 */
void ReadSessionTest::errorOnMailTransferMode()
{
    //Start to create a datagram with Read Request opcode as first byte
    QByteArray mailXferModeDatagram  = QByteArray::fromRawData(reinterpret_cast<char*>(&m_rrqOpcode), sizeof(m_rrqOpcode));
    mailXferModeDatagram.append("testfile.txt");   //append name of requested file
    mailXferModeDatagram.append(char(0x0));        //append terminating \0 of filename
    mailXferModeDatagram.append("Mail");           //append transfer mode
    mailXferModeDatagram.append(char(0x0));        //append terminating \0 of transfer mode

    //Note: IP address passed to createReadSessionAndReturnNetworkResponse is not used because we use stub socket classes.
    //So it doesn't matter if IP address below is different from your test computer.
    QByteArray sentData = createReadSessionAndReturnNetworkResponse(QHostAddress("10.6.11.123"), 1234, mailXferModeDatagram);

    const uint16_t* sentDataAsWords = reinterpret_cast<const uint16_t*>(sentData.constData());
    uint16_t opCode = sentDataAsWords[0];
    QCOMPARE(opCode, htons(0x0005));              //0x05 == error
    uint16_t errorCode = sentDataAsWords[1];
    QCOMPARE(errorCode, htons(0x0004));           //0x04 == illegal opcode
    QString errorMsg( sentData.constData() + 4 );
    bool errMsgCorrect = errorMsg == "Mail transfer not supported";
    QCOMPARE(errMsgCorrect, true);
    QCOMPARE(m_readSession->state(), Session::State::InError);
}


void ReadSessionTest::errorOnIllegalTransferMode()
{
    QByteArray illegalModeDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_rrqOpcode), sizeof(m_rrqOpcode));
    illegalModeDatagram.append("testfile.txt");    //name of requested file
    illegalModeDatagram.append(char(0x0));         //terminating \0 of filename
    illegalModeDatagram.append("compressed");      //non-existent transfer mode
    illegalModeDatagram.append(char(0x0));         //terminating \0 of transfer mode

    QByteArray sentData = createReadSessionAndReturnNetworkResponse(QHostAddress("10.6.11.123"), 1234, illegalModeDatagram);

    const uint16_t* sentDataAsWords = reinterpret_cast<const uint16_t*>(sentData.constData());
    uint16_t opCode = sentDataAsWords[0];
    QCOMPARE(opCode, htons(0x0005));              //0x05 == error
    uint16_t errorCode = sentDataAsWords[1];
    QCOMPARE(errorCode, htons(0x0004));           //0x04 == illegal opcode
    QString errorMsg( sentData.constData() + 4 );
    bool errMsgCorrect = errorMsg == "Illegal transfer mode";
    QCOMPARE(errMsgCorrect, true);
    QCOMPARE(m_readSession->state(), Session::State::InError);
}


void ReadSessionTest::errorOnNonExistingFile()
{
    QByteArray rrqDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_rrqOpcode), sizeof(m_rrqOpcode));
    rrqDatagram.append("i_dont_exist.txt"); //name of requested file
    rrqDatagram.append(char(0x0));          //terminating \0 of filename
    rrqDatagram.append("octet");            //transfer mode
    rrqDatagram.append(char(0x0));          //terminating \0 of transfer mode

    //Note: IP address passed to createReadSessionAndReturnNetworkResponse is not used because we use stub socket classes.
    //So it doesn't matter if IP address below is different from your test computer.
    QByteArray sentData = createReadSessionAndReturnNetworkResponse(QHostAddress("10.6.11.123"), 1234, rrqDatagram);

    const uint16_t* sentDataAsWords = reinterpret_cast<const uint16_t*>(sentData.constData());
    uint16_t opCode = sentDataAsWords[0];
    QCOMPARE(opCode, htons(0x0005));       //0x05 == error
    uint16_t errorCode = sentDataAsWords[1];
    QCOMPARE(errorCode, htons(0x0001));    //0x01 == FileNotFound
    QString errorMsg( sentData.constData() + 4 );
    bool errMsgCorrect = errorMsg == "File not found";
    QCOMPARE(errMsgCorrect, true);
    QCOMPARE(m_readSession->state(), Session::State::InError);
}


//NOTE: for this test the file "no_permission.txt" in the test_files directory should NOT be readable for the user
//      that runs this test (so don't run this as root ;-) )
void ReadSessionTest::errorOnNonReadableFile()
{
    QByteArray rrqDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_rrqOpcode), sizeof(m_rrqOpcode));
    rrqDatagram.append("no_permission.txt"); // name of requested file
    rrqDatagram.append(char(0x0));           // terminating \0 of filename
    rrqDatagram.append("octet");             // transfer mode
    rrqDatagram.append(char(0x0));           // terminating \0 of transfer mode

    QByteArray sentData = createReadSessionAndReturnNetworkResponse(QHostAddress("10.6.11.123"), 1234, rrqDatagram);

    const uint16_t* sentDataAsWords = reinterpret_cast<const uint16_t*>(sentData.constData());
    uint16_t opCode = sentDataAsWords[0];
    QCOMPARE(opCode, htons(0x0005));        //0x05 == error
    uint16_t errorCode = sentDataAsWords[1];
    QCOMPARE(errorCode, htons(0x0000));     //0x00 == Undefined error
    QString errorMsg( sentData.constData() + 4 );
#ifdef _WIN32
    bool errMsgCorrect = errorMsg == "Access is denied.";
#else
    bool errMsgCorrect = errorMsg == "Permission denied";
#endif
    QCOMPARE(errMsgCorrect, true);
    QCOMPARE(m_readSession->state(), Session::State::InError);
}


void ReadSessionTest::transferFileSmallerThanOneBlockBinary()
{
    QByteArray rrqDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_rrqOpcode), sizeof(m_rrqOpcode));
    rrqDatagram.append("16_byte_file.txt");  // name of requested file
    rrqDatagram.append(char(0x0));           // terminating \0 of filename
    rrqDatagram.append("octet");             // transfer mode
    rrqDatagram.append(char(0x0));           // terminating \0 of transfer mode

    //Note: IP address passed to createReadSessionAndReturnNetworkResponse is not used because we use stub socket classes.
    //So it doesn't matter if IP address below is different from your test computer.
    QByteArray sentData = createReadSessionAndReturnNetworkResponse(QHostAddress("10.6.11.123"), 1234, rrqDatagram);

    QCOMPARE(m_readSession->state(), Session::State::Busy);  //no ack received yet, so session not yet finished

    const uint16_t* sentDataAsWords = reinterpret_cast<const uint16_t*>(sentData.constData());
    uint16_t opCode = sentDataAsWords[0];
    QCOMPARE(opCode, htons(0x0003));        //0x03 == data packet
    uint16_t blockNr = sentDataAsWords[1];
    QCOMPARE(blockNr, htons(0x0001));
    QString fileContents( sentData.constData() + 4);
    bool contentsCorrect = fileContents == "Elvis is alive!\n";
    QCOMPARE(contentsCorrect, true);

    //TODO: sent ack and check that state goes to Finished
}


void ReadSessionTest::transferFileLargerThanOneBlockBinary()
{
    QByteArray rrqDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_rrqOpcode), sizeof(m_rrqOpcode));
    rrqDatagram.append("600_byte_file.txt"); // name of requested file
    rrqDatagram.append(char(0x0));           // terminating \0 of filename
    rrqDatagram.append("octet");             // transfer mode
    rrqDatagram.append(char(0x0));           // terminating \0 of transfer mode

    QByteArray sentData = createReadSessionAndReturnNetworkResponse(QHostAddress("10.6.11.123"), 1234, rrqDatagram);

    QCOMPARE(m_readSession->state(), Session::State::Busy);

    const uint16_t* sentDataAsWords = reinterpret_cast<const uint16_t*>(sentData.constData());
    uint16_t opCode = sentDataAsWords[0];
    QCOMPARE(opCode, htons(0x0003));        //0x03 == data packet
    uint16_t blockNr = sentDataAsWords[1];
    QCOMPARE(blockNr, htons(0x0001));

    //save the first data block that was sent in a byte array
    QByteArray sentBlock = sentData.right( sentData.size() - 4 );

    //read the same amount of bytes directly from the test file
    QByteArray fileBlock;
    auto bytesRead = readBytesFromFile(fileBlock, "600_byte_file.txt", 0, DefaultTftpBlockSize);
    QCOMPARE( bytesRead>0, true); //test for read failure first, because we have to cast bytesRead to unsigned int
    QCOMPARE(bytesRead, DefaultTftpBlockSize);

    //and make sure the two byte arrays are the same
    QCOMPARE(sentBlock, fileBlock);

    //send ack to readsession
    SimulatedNetworkStream &inNetworkStream = m_socketFactory->getNetworkStreamBySource(UdpSocketStubFactory::StreamDirection::Input, QHostAddress::Any, 0);
    QByteArray ackDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_ackOpcode), sizeof(m_ackOpcode));
    uint16_t ackBlockNr = htons( 0x0001);
    ackDatagram.append(reinterpret_cast<const char*>(&ackBlockNr), sizeof(ackBlockNr));
    inNetworkStream << ackDatagram;

    QCOMPARE(m_readSession->state(), Session::State::Busy); // final ack not yet received, so state should still be 'busy'

    //send duplicate ack to readsession, should be ignored by readsession
    //SimulatedNetworkStream &inNetworkStream = m_socketFactory->getNetworkStreamBySource(UdpSocketStubFactory::StreamDirection::Input, QHostAddress::Any, 0);
    //QByteArray ackDatagram = QByteArray::fromRawData((char*)&m_ackOpcode, sizeof(m_ackOpcode));
    //uint16_t ackBlockNr = htons( 0x0001);
    //ackDatagram.append((const char*)&ackBlockNr, sizeof(ackBlockNr));
    inNetworkStream << ackDatagram;

    QCOMPARE(m_readSession->state(), Session::State::Busy); // final ack not yet received, so state should still be 'busy'

    //check opcode and block sequence nr in second datagram
    SimulatedNetworkStream &outNetworkStream = m_socketFactory->getNetworkStreamBySource(UdpSocketStubFactory::StreamDirection::Output, QHostAddress::Any, 0);
    outNetworkStream >> sentData;
    sentDataAsWords = reinterpret_cast<const uint16_t*>(sentData.constData()); //sentData was re-allocated so need to update our cast ptr
    opCode = sentDataAsWords[0];
    QCOMPARE(opCode, htons(0x0003));        //0x03 == data packet
    blockNr = sentDataAsWords[1];
    QCOMPARE(blockNr, htons(0x0002));       //0x02 -> second data block

    //save the second data block that we received in a byte array
    sentBlock = sentData.right( sentData.size() - 4 );

    //read the remaining data from the test file
    bytesRead = readBytesFromFile(fileBlock, "600_byte_file.txt", DefaultTftpBlockSize, DefaultTftpBlockSize);
    QCOMPARE( bytesRead>0, true); //test for read failure first, because we have to cast bytesRead to unsigned int
    QCOMPARE(bytesRead, 600 - DefaultTftpBlockSize);

    //and make sure the two byte arrays are the same
    QCOMPARE(sentBlock, fileBlock);

    //send final ack to readsession
    ackDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_ackOpcode), sizeof(m_ackOpcode));
    ackBlockNr = htons(0x0002);
    ackDatagram.append(reinterpret_cast<const char*>(&ackBlockNr), sizeof(ackBlockNr));
    inNetworkStream << ackDatagram;

    QCOMPARE(m_readSession->state(), Session::State::Finished);
}


void ReadSessionTest::transferFileExactMultipleOfBlockSizeBinary()
{
    QByteArray rrqDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_rrqOpcode), sizeof(m_rrqOpcode));
    rrqDatagram.append("1024_byte_file.txt"); // name of requested file
    rrqDatagram.append(char(0x0));           // terminating \0 of filename
    rrqDatagram.append("octet");             // transfer mode
    rrqDatagram.append(char(0x0));           // terminating \0 of transfer mode

    //Note: IP address passed to createReadSessionAndReturnNetworkResponse is not used because we use stub socket classes.
    //So it doesn't matter if IP address below is different from your test computer.
    QByteArray sentData = createReadSessionAndReturnNetworkResponse(QHostAddress("10.6.11.123"), 1234, rrqDatagram);
    //contents of first block already tested in previous tests

    //send ack to readsession
    SimulatedNetworkStream &inNetworkStream = m_socketFactory->getNetworkStreamBySource(UdpSocketStubFactory::StreamDirection::Input, QHostAddress::Any, 0);
    QByteArray ackDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_ackOpcode), sizeof(m_ackOpcode));
    uint16_t ackBlockNr = htons( 0x0001);
    ackDatagram.append(reinterpret_cast<const char*>(&ackBlockNr), sizeof(ackBlockNr));
    inNetworkStream << ackDatagram;
    QCOMPARE(m_readSession->state(), Session::State::Busy);

    //save the second data block that we received in a byte array
    //check opcode and block sequence nr in second datagram
    SimulatedNetworkStream &outNetworkStream = m_socketFactory->getNetworkStreamBySource(UdpSocketStubFactory::StreamDirection::Output, QHostAddress::Any, 0);
    outNetworkStream >> sentData;
    const uint16_t* sentDataAsWords = reinterpret_cast<const uint16_t*>(sentData.constData());
    sentDataAsWords = reinterpret_cast<const uint16_t*>(sentData.constData()); //sentData was re-allocated so need to update our cast ptr
    uint16_t opCode = sentDataAsWords[0];
    QCOMPARE(opCode, htons(0x0003));        //0x03 == data packet
    uint16_t blockNr = sentDataAsWords[1];
    QCOMPARE(blockNr, htons(0x0002));       //0x02 -> second data block
    QByteArray sentBlock = sentData.right( sentData.size() - 4 );

    //read the same block directly from the test file
    QByteArray expectedBlock;
    auto bytesRead = readBytesFromFile(expectedBlock, "1024_byte_file.txt", DefaultTftpBlockSize, DefaultTftpBlockSize);
    QCOMPARE( bytesRead>0, true); //test for read failure first, because we have to cast bytesRead to unsigned int
    QCOMPARE(bytesRead, DefaultTftpBlockSize);

    //and make sure the two byte arrays are the same
    QCOMPARE(sentBlock, expectedBlock);

    //send another ack to readsession
    ackDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_ackOpcode), sizeof(m_ackOpcode));
    ackBlockNr = htons( 0x0002);
    ackDatagram.append(reinterpret_cast<char*>(&ackBlockNr), sizeof(ackBlockNr));
    inNetworkStream << ackDatagram;

    //and make sure that we receive an empty data packet to finish the transfer
    outNetworkStream >> sentData;
    sentDataAsWords = reinterpret_cast<const uint16_t*>(sentData.constData()); //sentData was re-allocated so need to update our cast ptr
    opCode = sentDataAsWords[0];
    QCOMPARE(opCode, htons(0x0003));        //0x03 == data packet
    blockNr = sentDataAsWords[1];
    QCOMPARE(blockNr, htons(0x0003));       //0x03 -> third data block
    sentBlock = sentData.right( sentData.size() - 4 );
    QCOMPARE(sentBlock.size(), 0);

    QCOMPARE(m_readSession->state(), Session::State::Busy); //final ack not yet received

    //send another ack to readsession to confirm receipt of empty data block
    ackDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_ackOpcode), sizeof(m_ackOpcode));
    ackBlockNr = htons( 0x0003);
    ackDatagram.append(reinterpret_cast<const char*>(&ackBlockNr), sizeof(ackBlockNr));
    inNetworkStream << ackDatagram;

    outNetworkStream >> sentData;
    QCOMPARE(sentData.size(), 0); //readsession should not respond to final ACK
    QCOMPARE(m_readSession->state(), Session::State::Finished);
}


void ReadSessionTest::retransmitDataBlockOnTimeout()
{
    //reduce the timeout value for re-transmission to keep our unit test execution time short
    ReadSession::setRetransmitTimeOut(30);

    QByteArray rrqDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_rrqOpcode), sizeof(m_rrqOpcode));
    rrqDatagram.append("600_byte_file.txt"); // name of requested file
    rrqDatagram.append(char(0x0));           // terminating \0 of filename
    rrqDatagram.append("octet");             // transfer mode
    rrqDatagram.append(char(0x0));           // terminating \0 of transfer mode

    QByteArray sentData = createReadSessionAndReturnNetworkResponse(QHostAddress("10.6.11.123"), 1234, rrqDatagram);
    //contents of first block already tested in previous tests

    QTest::qWait(180); // simulate no response from client after initial transmission and DefaultMaxRetryCount retries

    //test that first block of file was sent again DefaultMaxRetryCount times
    SimulatedNetworkStream &outNetworkStream = m_socketFactory->getNetworkStreamBySource(UdpSocketStubFactory::StreamDirection::Output, QHostAddress::Any, 0);
    outNetworkStream >> sentData;
    if (sentData.size() != DefaultMaxRetryCount*(DefaultTftpBlockSize+4))
    {
        QString msg = QString("Retransmissions have wrong packet size or wrong nr of retransmissions. Total network output: actual: %2, expected: %3").arg(
                    QString::number(sentData.size()), QString::number( DefaultMaxRetryCount*(DefaultTftpBlockSize+4) ) );
        QFAIL(static_cast<const char*>(msg.toLatin1()));
    }
    //read the same amount of file bytes that we expect directly from the test file
    QByteArray expectedBlock;
    auto bytesRead = readBytesFromFile(expectedBlock, "600_byte_file.txt", 0, DefaultTftpBlockSize);
    QCOMPARE(bytesRead, DefaultTftpBlockSize);

    for (unsigned int retryCount=1; retryCount<=DefaultMaxRetryCount; ++retryCount)
    {
        const uint16_t* sentDataAsWords = reinterpret_cast<const uint16_t*>(sentData.constData());
        uint16_t opCode = sentDataAsWords[0];
        QCOMPARE(opCode, htons(0x0003));        //0x03 == data packet
        uint16_t blockNr = sentDataAsWords[1];
        QCOMPARE(blockNr, htons(0x0001));       //0x01 == first data block of requested file

        //save the first data block that was sent (for the second time) in a byte array
        QByteArray sentBlock = sentData.mid( 4, DefaultTftpBlockSize );

        //and make sure the two byte arrays are the same
        if (sentBlock != expectedBlock)
        {
            QString msg = QString("Retransmission %1 has wrong data block.").arg(QString::number(retryCount));
            QFAIL(static_cast<const char*>(msg.toLatin1()));
        }

        sentData = sentData.mid(DefaultTftpBlockSize+4); //remove the packet that we just checked from the network output
    }

    QCOMPARE(sentData.size(), 0); //check that data block was retransmitted no more than DefaultMaxRetryCount times

    QCOMPARE(m_readSession->state(), Session::State::InError);

}


//Unit test created for issue YMP-113: retransmission stopped after total retransmissions exceeds max iso only retransmissions of last package
void ReadSessionTest::retransmitMultiplePackets()
{
    //reduce the timeout value for re-transmission to keep our unit test execution time short
    ReadSession::setRetransmitTimeOut(30);
    ReadSession::setMaxRetransmissions(3);

    QByteArray rrqDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_rrqOpcode), sizeof(m_rrqOpcode));
    rrqDatagram.append("large_file.txt"); // name of requested file
    rrqDatagram.append(char(0x0));           // terminating \0 of filename
    rrqDatagram.append("octet");             // transfer mode
    rrqDatagram.append(char(0x0));           // terminating \0 of transfer mode

    QByteArray sentData = createReadSessionAndReturnNetworkResponse(QHostAddress("10.6.11.123"), 1234, rrqDatagram);
    //contents of first block already tested in previous tests

    QTest::qWait(80); // simulate no response from client after initial transmission, readsession should retransmit twice

    //test that first block of file was sent again twice
    SimulatedNetworkStream &outNetworkStream = m_socketFactory->getNetworkStreamBySource(UdpSocketStubFactory::StreamDirection::Output, QHostAddress::Any, 0);
    outNetworkStream >> sentData;
    if (sentData.size() != 2*(DefaultTftpBlockSize+4))
    {
        QString msg = QString("Retransmissions have wrong packet size or wrong nr of retransmissions. Total network output: actual: %1, expected: %2").arg(
                    QString::number(sentData.size()), QString::number( 2*(DefaultTftpBlockSize+4) ) );
        QFAIL(static_cast<const char*>(msg.toLatin1()));
    }

    //send ack to readsession
    SimulatedNetworkStream &inNetworkStream = m_socketFactory->getNetworkStreamBySource(UdpSocketStubFactory::StreamDirection::Input, QHostAddress::Any, 0);
    QByteArray ackDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_ackOpcode), sizeof(m_ackOpcode));
    uint16_t ackBlockNr = htons( 0x0001);
    ackDatagram.append(reinterpret_cast<const char*>(&ackBlockNr), sizeof(ackBlockNr));
    inNetworkStream << ackDatagram;

    QTest::qWait(180); // simulate no response from client after initial transmission, readsession should retransmit 3 times

    //test that second block of file was sent again DefaultMaxRetryCount times
    SimulatedNetworkStream &outNetworkStream2 = m_socketFactory->getNetworkStreamBySource(UdpSocketStubFactory::StreamDirection::Output, QHostAddress::Any, 0);
    outNetworkStream2 >> sentData;
    if (sentData.size() != 4*(DefaultTftpBlockSize+4))
    {
        unsigned dummy = 4*(DefaultTftpBlockSize+4);
        QString msg = QString("Retransmissions have wrong packet size or wrong nr of retransmissions. Total network output: actual: %1, expected: %2").arg(
                    QString::number(sentData.size()), QString::number( 4*(DefaultTftpBlockSize+4) ) );
        QFAIL(static_cast<const char*>(msg.toLatin1()));
    }
}


void ReadSessionTest::transmitFileSmallerThanOneBlockAscii()
{
    QByteArray rrqDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_rrqOpcode), sizeof(m_rrqOpcode));
    rrqDatagram.append("different_line_endings.txt");  // name of requested file
    rrqDatagram.append(char(0x0));           // terminating \0 of filename
    rrqDatagram.append("netascii");          // transfer mode
    rrqDatagram.append(char(0x0));           // terminating \0 of transfer mode

    //Note: IP address passed to createReadSessionAndReturnNetworkResponse is not used because we use stub socket classes.
    //So it doesn't matter if IP address below is different from your test computer.
    QByteArray sentData = createReadSessionAndReturnNetworkResponse(QHostAddress("10.6.11.123"), 1234, rrqDatagram);

    QByteArray fileContents( sentData.constData() + 4, sentData.size()-4);
    //The characters from the file with correct CR-LF conversions
    const char *convertedContentsHex = "5468697320697320612066696C6520776974680D000D0A646966666572656E742074797065730D0A6F660D0A6C696E6520656E64696E677320616E640D000D0A63617272696167652072657475726E0D0A636861726163746572732E0D000D000D0A0D0A0D0A0D00";
    QByteArray expectedContents = QByteArray::fromHex(convertedContentsHex);
    QCOMPARE(fileContents, expectedContents);
}


void ReadSessionTest::transmitFileLargerThanOneBlockAscii()
{
    QByteArray rrqDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_rrqOpcode), sizeof(m_rrqOpcode));
    rrqDatagram.append("different_line_endings_2blocks.txt"); // name of requested file
    rrqDatagram.append(char(0x0));           // terminating \0 of filename
    rrqDatagram.append("netascii");          // transfer mode
    rrqDatagram.append(char(0x0));           // terminating \0 of transfer mode

    QByteArray sentData = createReadSessionAndReturnNetworkResponse(QHostAddress("10.6.11.123"), 1234, rrqDatagram);

    //save the first data block that was sent in a byte array
    QByteArray sentBlock = sentData.right( sentData.size() - 4 );

    //read the expected block contents from file and make sure transmitted block is identical
    QByteArray expectedHexArray;
    //we have to read DefaultTftpBlockSize*2 characters from hex file because each byte is dumped as two ascii characters (0-9,A-F)
    auto bytesRead = readBytesFromFile(expectedHexArray, "different_line_endings_2blocks_asciitransfer_block1_expected.txt", 0, DefaultTftpBlockSize*2);
    QCOMPARE( bytesRead>0, true);
    QByteArray expectedBlock = QByteArray::fromHex(expectedHexArray);
    QCOMPARE(sentBlock, expectedBlock);

    //send ACK for first data block to read session so that 2nd block will be transferred
    SimulatedNetworkStream &inNetworkStream = m_socketFactory->getNetworkStreamBySource(UdpSocketStubFactory::StreamDirection::Input, QHostAddress::Any, 0);
    QByteArray ackDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_ackOpcode), sizeof(m_ackOpcode));
    uint16_t ackBlockNr = htons( 0x0001);
    ackDatagram.append(reinterpret_cast<const char*>(&ackBlockNr), sizeof(ackBlockNr));
    inNetworkStream << ackDatagram;
    SimulatedNetworkStream &outNetworkStream = m_socketFactory->getNetworkStreamBySource(UdpSocketStubFactory::StreamDirection::Output, QHostAddress::Any, 0);
    outNetworkStream >> sentData;

    //save the second data block that we received in a byte array
    sentBlock = sentData.right( sentData.size() - 4 );

    //read the expected contents for block 2 from file and make sure transmitted block is identical
    bytesRead = readBytesFromFile(expectedHexArray, "different_line_endings_2blocks_asciitransfer_block2_expected.txt", 0, DefaultTftpBlockSize*2);
    QCOMPARE( bytesRead>0, true);
    expectedBlock = QByteArray::fromHex(expectedHexArray);
    QCOMPARE(sentBlock, expectedBlock);
}

void ReadSessionTest::detectSlowNetwork()
{
    QByteArray rrqDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_rrqOpcode), sizeof(m_rrqOpcode));
    rrqDatagram.append("large_file.txt"); // name of requested file
    rrqDatagram.append(char(0x0));           // terminating \0 of filename
    rrqDatagram.append("netascii");          // transfer mode
    rrqDatagram.append(char(0x0));           // terminating \0 of transfer mode

    bool signalEmitted = false;
    QByteArray sentData = createReadSessionAndReturnNetworkResponse(QHostAddress("10.6.11.123"), 1234, rrqDatagram);
    connect(m_readSession.get(), &ReadSession::slowNetwork, [&]() { signalEmitted=true;} );

    SimulatedNetworkStream &inNetworkStream = m_socketFactory->getNetworkStreamBySource(UdpSocketStubFactory::StreamDirection::Input, QHostAddress::Any, 0);
    SimulatedNetworkStream &outNetworkStream = m_socketFactory->getNetworkStreamBySource(UdpSocketStubFactory::StreamDirection::Output, QHostAddress::Any, 0);
    for (int i=0; i<6; ++i)
    {
        //sleep 1 ms to simulate somewhat slow network
        using namespace std::chrono_literals;
        std::this_thread::sleep_for(1ms);
        QByteArray ackDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_ackOpcode), sizeof(m_ackOpcode));
        uint16_t ackBlockNr = htons(i);
        ackDatagram.append(reinterpret_cast<const char*>(&ackBlockNr), sizeof(ackBlockNr));
        inNetworkStream << ackDatagram;
        outNetworkStream >> sentData;
    }

    //Average response time should still be below 2000 us default threshold, so slowNetwork signal should not be emitted yet
    QCOMPARE(signalEmitted, false);

    for (int i=6; i<12; ++i)
    {
        //sleep 10 ms to simulate slow network
        using namespace std::chrono_literals;
        std::this_thread::sleep_for(10ms);
        QByteArray ackDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_ackOpcode), sizeof(m_ackOpcode));
        uint16_t ackBlockNr = htons(i);
        ackDatagram.append(reinterpret_cast<const char*>(&ackBlockNr), sizeof(ackBlockNr));
        inNetworkStream << ackDatagram;
        outNetworkStream >> sentData;
    }

    //Now the average respons time should have dropped enough to emit slowNetwork signal
    QCOMPARE(signalEmitted, true);
}

void ReadSessionTest::transmitOackOnOptionsRrq()
{
    QByteArray rrqDatagram = QByteArray::fromRawData(reinterpret_cast<char*>(&m_rrqOpcode), sizeof(m_rrqOpcode));
    rrqDatagram.append("16_byte_file.txt");  // name of requested file
    rrqDatagram.append(char(0x0));           // terminating \0 of filename
    rrqDatagram.append("octet");             // transfer mode
    rrqDatagram.append(char(0x0));           // terminating \0 of transfer mode
    rrqDatagram.append("blksize");           // name of blocksize option
    rrqDatagram.append(char(0x0));           // terminating \0 of blocksize option name
    rrqDatagram.append("1024");              // value of blocksize option
    rrqDatagram.append(char(0x0));           // terminating \0 of blocksize option value
    rrqDatagram.append("timeout");           // name of timeout option
    rrqDatagram.append(char(0x0));           // terminating \0 of timeout option name
    rrqDatagram.append("1");                 // value of timeout option
    rrqDatagram.append(char(0x0));           // terminating \0 of timeout option value
    rrqDatagram.append("tsize");             // name of transfer size option
    rrqDatagram.append(char(0x0));           // terminating \0 of transfer size option name
    rrqDatagram.append("0");                 // value of tsize option
    rrqDatagram.append(char(0x0));           // terminating \0 of transfer size option value

    //Note: IP address passed to createReadSessionAndReturnNetworkResponse is not used because we use stub socket classes.
    //So it doesn't matter if IP address below is different from your test computer.
    QByteArray sentData = createReadSessionAndReturnNetworkResponse(QHostAddress("10.6.11.123"), 1234, rrqDatagram);

    QCOMPARE(m_readSession->state(), Session::State::OptionsNegotation);  //readsession waiting for client to acknowledge Oack
    QCOMPARE(sentData.size() == 34, true);

    const uint16_t* sentDataAsWords = reinterpret_cast<const uint16_t*>(sentData.constData());
    uint16_t opCode = sentDataAsWords[0];
    QCOMPARE(opCode, htons(0x0006));        //0x06 == Options Acknowledgement
    int offset = 2;
    bool blockSizeAck=false, timeoutAck=false, tsizeAck=false;
    while (offset < sentData.size())
    {
        QString option1Name( sentData.constData() + offset);
        offset += option1Name.size() + 1;
        QString option1Value = sentData.constData()+ offset;
        offset += option1Value.size() + 1;

        auto fileSize = QFileInfo(QDir(TFTP_TEST_FILES_DIR), "16_byte_file.txt").size();
        if (option1Name == "blksize")
        {
            QCOMPARE(blockSizeAck, false); //option should be acknowledged only once
            QCOMPARE(option1Value, QString("1024"));
            blockSizeAck=true;
        }
        else if (option1Name == "timeout")
        {
            QCOMPARE(timeoutAck, false);
            QCOMPARE(option1Value, QString("1"));
            timeoutAck = true;
        }
        else if (option1Name == "tsize")
        {
            QCOMPARE(tsizeAck, false);
            QCOMPARE(option1Value, QString::number(fileSize));
            tsizeAck = true;
        }
    }

    QCOMPARE(timeoutAck && blockSizeAck && tsizeAck, true);

}

//TODO: test ascii transfer mode with CR as last byte of full block

} // namespace QTFTP end

QTEST_MAIN(QTFTP::ReadSessionTest)
#include "readsession_ut.moc"


