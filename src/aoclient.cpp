//////////////////////////////////////////////////////////////////////////////////////
//    akashi - a server for Attorney Online 2                                       //
//    Copyright (C) 2020  scatterflower                                             //
//                                                                                  //
//    This program is free software: you can redistribute it and/or modify          //
//    it under the terms of the GNU Affero General Public License as                //
//    published by the Free Software Foundation, either version 3 of the            //
//    License, or (at your option) any later version.                               //
//                                                                                  //
//    This program is distributed in the hope that it will be useful,               //
//    but WITHOUT ANY WARRANTY; without even the implied warranty of                //
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                 //
//    GNU Affero General Public License for more details.                           //
//                                                                                  //
//    You should have received a copy of the GNU Affero General Public License      //
//    along with this program.  If not, see <https://www.gnu.org/licenses/>.        //
//////////////////////////////////////////////////////////////////////////////////////
#include "include/aoclient.h"

AOClient::AOClient(Server* p_server, QTcpSocket* p_socket, QObject* parent, int user_id)
    : QObject(parent)
{
    socket = p_socket;
    server = p_server;
    id = user_id;
    joined = false;
    password = "";
    current_area = 0;
    current_char = "";
    remote_ip = p_socket->peerAddress();
    is_partial = false;
    last_wtce_time = 0;
    last_message = "";
}

void AOClient::clientData()
{
    QString data = QString::fromUtf8(socket->readAll());

    if (is_partial) {
        data = partial_packet + data;
    }
    if (!data.endsWith("%")) {
        is_partial = true;
    }

    QStringList all_packets = data.split("%");
    all_packets.removeLast(); // Remove the entry after the last delimiter

    for (QString single_packet : all_packets) {
        AOPacket packet(single_packet);
        handlePacket(packet);
    }
}

void AOClient::clientDisconnected()
{
#ifdef NET_DEBUG
    qDebug() << remote_ip.toString() << "disconnected";
#endif
    if (joined) {
        server->player_count--;
        server->areas[current_area]->player_count--;
        arup(ARUPType::PLAYER_COUNT, true);
    }
    if (current_char != "") {
        server->areas[current_area]->characters_taken[current_char] =
            false;
        server->updateCharsTaken(server->areas[current_area]);
    }
    for (AreaData* area : server->areas) {
        area->owners.removeAll(id);
        area->invited.removeAll(id);
    }
    arup(ARUPType::CM, true);
}

void AOClient::handlePacket(AOPacket packet)
{
#ifdef NET_DEBUG
    qDebug() << "Received packet:" << packet.header << ":" << packet.contents << "args length:" << packet.contents.length();
#endif
    AreaData* area = server->areas[current_area];
    PacketInfo info = packets.value(packet.header, {false, 0, &AOClient::pktDefault});

    if (!checkAuth(info.acl_mask)) {
        return;
    }

    if (packet.contents.length() < info.minArgs) {
#ifdef NET_DEBUG
        qDebug() << "Invalid packet args length. Minimum is" << info.minArgs << "but only" << packet.contents.length() << "were given.";
#endif
        return;
    }

    (this->*(info.action))(area, packet.contents.length(), packet.contents, packet);
}

void AOClient::changeArea(int new_area)
{
    if (current_area == new_area) {
        sendServerMessage("You are already in area " + server->area_names[current_area]);
        return;
    }
    if (server->areas[new_area]->locked) {
        sendServerMessage("Area " + server->area_names[new_area] + " is locked.");
        return;
    }
    if (current_char != "") {
        server->areas[current_area]->characters_taken[current_char] =
            false;
        server->updateCharsTaken(server->areas[current_area]);
    }
    server->areas[new_area]->player_count++;
    server->areas[current_area]->player_count--;
    current_area = new_area;
    arup(ARUPType::PLAYER_COUNT, true);
    sendEvidenceList(server->areas[new_area]);
    sendPacket("HP", {"1", QString::number(server->areas[new_area]->def_hp)});
    sendPacket("HP", {"2", QString::number(server->areas[new_area]->pro_hp)});
    sendPacket("BN", {server->areas[new_area]->background});
    if (server->areas[current_area]->characters_taken[current_char]) {
        server->updateCharsTaken(server->areas[current_area]);
        current_char = "";
        sendPacket("DONE");
    }
    else {
        server->areas[current_area]->characters_taken[current_char] = true;
        server->updateCharsTaken(server->areas[current_area]);
    }
    sendServerMessage("You have been moved to area " + server->area_names[current_area]);
}

void AOClient::handleCommand(QString command, int argc, QStringList argv)
{
    CommandInfo info = commands.value(command, {false, -1, &AOClient::cmdDefault});

    if (!checkAuth(info.acl_mask)) {
        sendServerMessage("You do not have permission to use that command.");
        return;
    }

    if (argc < info.minArgs) {
        sendServerMessage("Invalid command syntax.");
        return;
    }

    (this->*(info.action))(argc, argv);
}

void AOClient::arup(ARUPType type, bool broadcast)
{
    QStringList arup_data;
    arup_data.append(QString::number(type));
    for (AreaData* area : server->areas) {
        if (type == ARUPType::PLAYER_COUNT) {
            arup_data.append(QString::number(area->player_count));
        }
        else if (type == ARUPType::STATUS) {
            arup_data.append(area->status);
        }
        else if (type == ARUPType::CM) {
            if (area->owners.isEmpty())
                arup_data.append("FREE");
            else {
                QStringList area_owners;
                for (int owner_id : area->owners) {
                    AOClient* owner = server->getClientByID(owner_id);
                    area_owners.append("[" + QString::number(owner->id) + "] " + owner->current_char);
                    }
                arup_data.append(area_owners.join(", "));
                }
            }
        else if (type == ARUPType::LOCKED) {
            QString lock_status;
            switch (area->locked) {
                case FREE:
                    lock_status = "FREE";
                    break;
                case LOCKED:
                    lock_status = "LOCKED";
                    break;
                case SPECTATABLE:
                    lock_status = "SPECTATABLE";
                    break;
                default:
                    break;
            }
            arup_data.append(lock_status);
        }
        else return;
    }
    if (broadcast)
        server->broadcast(AOPacket("ARUP", arup_data));
    else
        sendPacket("ARUP", arup_data);
}

void AOClient::fullArup() {
    arup(ARUPType::PLAYER_COUNT, false);
    arup(ARUPType::STATUS, false);
    arup(ARUPType::CM, false);
    arup(ARUPType::LOCKED, false);
}

void AOClient::sendPacket(AOPacket packet)
{
#ifdef NET_DEBUG
    qDebug() << "Sent packet:" << packet.header << ":" << packet.contents;
#endif
    socket->write(packet.toUtf8());
    socket->flush();
}

void AOClient::sendPacket(QString header, QStringList contents)
{
    sendPacket(AOPacket(header, contents));
}

void AOClient::sendPacket(QString header)
{
    sendPacket(AOPacket(header, {}));
}

QString AOClient::getHwid() { return hwid; }

void AOClient::setHwid(QString p_hwid)
{
    // TODO: add support for longer hwids?
    // This reduces the (fairly high) chance of
    // birthday paradox issues arising. However,
    // typing more than 8 characters might be a
    // bit cumbersome.
    hwid = p_hwid;

    QCryptographicHash hash(
        QCryptographicHash::Md5); // Don't need security, just
                                  // hashing for uniqueness
    QString concat_ip_id = remote_ip.toString() + p_hwid;
    hash.addData(concat_ip_id.toUtf8());

    ipid = hash.result().toHex().right(8); // Use the last 8 characters (4 bytes)
}

void AOClient::sendServerMessage(QString message)
{
    sendPacket("CT", {"Server", message, "1"});
}

bool AOClient::checkAuth(unsigned long long acl_mask)
{
    if (acl_mask != ACLFlags.value("NONE")) {
        if (!authenticated) {
            return false;
        }

        QSettings settings("config/config.ini", QSettings::IniFormat);
        settings.beginGroup("Options");
        QString auth_type = settings.value("auth", "simple").toString();
        if (auth_type == "advanced") {
            unsigned long long user_acl = server->db_manager->getACL(moderator_name);
            return (user_acl & acl_mask) != 0;
        }
        else if (auth_type == "simple") {
            return authenticated;
        }
    }
    return true;
}

QString AOClient::getIpid() { return ipid; }

Server* AOClient::getServer() { return server; };

AOClient::~AOClient() {
    socket->deleteLater();
}
