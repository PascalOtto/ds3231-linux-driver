#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/bcd.h>
#include <linux/rtc.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <asm/errno.h>
#include <asm/delay.h>


/* Register Definitionen */
#define DS3231_REG_SECONDS      0x00
#define DS3231_REG_MINUTES      0x01
#define DS3231_REG_HOURS        0x02
# define DS3231_BIT_12H         0x40
# define DS3231_BIT_nAM         0x20
#define DS3231_REG_DAY          0x03
#define DS3231_REG_DATE         0x04
#define DS3231_REG_MONTH        0x05
# define DS3231_BIT_CENTURY     0x80
#define DS3231_REG_YEAR         0x06
#define DS3231_REG_CONTROL      0x0e
# define DS3231_BIT_nEOSC       0x80
# define DS3231_BIT_INTCN       0x04
# define DS3231_BIT_A2IE        0x02
# define DS3231_BIT_A1IE        0x01
#define DS3231_REG_STATUS       0x0f
# define DS3231_BIT_OSF         0x80



/*
 * Der Zeiger wird bei Initialisierung gesetzt und wird für die
 * i2c Kommunikation mit dem  Device (DS3231) benötigt.
 */
static struct i2c_client *ds3231_client;


/*
 * Mutex für den I2C-Zugriff.
 */
static DEFINE_MUTEX(i2c_lock);


/* --------------------------------------------------------------------------------------------------------
    Zugriff auf Register des DS3231 und Hilfsfunktionen zur Datumsformatierung
   --------------------------------------------------------------------------------------------------------*/


/*
 * Liest mehrere Bytes aus dem Register
 */
static s32 ds3231_read_block_data(u8 reg, u8 len, u8 *buf)
{
    s32 i, data;

    for(i = 0; i < len; i++) {
        data = i2c_smbus_read_byte_data(ds3231_client, reg + i);
        if(data < 0) {
            printk("DS3231_drv: Kann Register 0x%02x nicht lesen (errorn = %d).\n", reg + i, data);
            return data;
        }
        buf[i] = (u8)data;
    }
    return i;
}


/*
 * Schreibt mehrere Bytes in das Register
 */
static s32 ds3231_write_block_data(u8 reg, u8 len, u8 *buf)
{
    s32 i, err;

    for(i = 0; i < len; i++) {
        err = i2c_smbus_write_byte_data(ds3231_client, reg + i, buf[i]);
        if(err < 0) {
            printk("DS3231_drv: Kann Register 0x%02x nicht beschreiben (errorn = %d).\n", reg + i, err);
            return err;
        }
    }
    return 0;
}


/*
 * Datum aus dem Register auslesen und in Struct zurueckgeben
 */
static s32 ds3231_read_date(struct rtc_time *date) 
{
    s32 ret;
    u8 regs[7];
    u8 hour_pm = 0;

    mutex_lock(&i2c_lock);
    ret = ds3231_read_block_data(DS3231_REG_SECONDS, sizeof(regs), regs);
    mutex_unlock(&i2c_lock);
    if(ret < 0) {
        return ret;
    }

    /* Registerwerte verwerten */
    /* ---Date--- */
    date->tm_mday = bcd2bin(regs[DS3231_REG_DATE]);

    /* ---Month--- */
    if(regs[DS3231_REG_MONTH] & DS3231_BIT_CENTURY) {
        printk("DS3231_drv: Century-Bit ist gesetzt");
        return EINVAL;
    }
    date->tm_mon = bcd2bin(regs[DS3231_REG_MONTH]) - 1;

    /* ---Year--- */
    date->tm_year = bcd2bin(regs[DS3231_REG_YEAR]) + 100;

    /* ---Hour--- */
    /*
     * 24H Format
     */
     if(regs[DS3231_REG_HOURS] & DS3231_BIT_12H) {
        regs[DS3231_REG_HOURS] &= ~DS3231_BIT_12H;
        if(regs[DS3231_REG_HOURS] & DS3231_BIT_nAM) {
            hour_pm = 12;
            regs[DS3231_REG_HOURS] &= ~DS3231_BIT_nAM;
        }
    }
    date->tm_hour = bcd2bin(regs[DS3231_REG_HOURS]) + hour_pm;

    /* ---Minute--- */
    date->tm_min = bcd2bin(regs[DS3231_REG_MINUTES]);

    /* --- Seconds --- */
    date->tm_sec = bcd2bin(regs[DS3231_REG_SECONDS]);

    return 0;
}


/*
 * Datum in das Register schreiben
 */
static s32 ds3231_write_date(const struct rtc_time *date) 
{
    u8 regs[7];
    s32 ret;
    int hour = date->tm_hour;

    mutex_lock(&i2c_lock);
    ret = ds3231_read_block_data(DS3231_REG_SECONDS, sizeof(regs), regs);
    mutex_unlock(&i2c_lock);
    if(ret < 0) {
        return ret;
    }

    /* Datum konvertieren */
    /* ---Date--- */
    regs[DS3231_REG_DATE] = bin2bcd((u8) date->tm_mday & 0x3f);

    /* ---Month--- */
    regs[DS3231_REG_MONTH] &= ~0x9f;
    regs[DS3231_REG_MONTH] |= bin2bcd((u8)(date->tm_mon + 1) & 0x1f);

    /* ---Year--- */
    if( date->tm_year > 199 ){
        regs[DS3231_REG_MONTH] |= 0x80; // set century bit
    }
    regs[DS3231_REG_YEAR] = bin2bcd((u8)(date->tm_year%100) & 0xff);

    /* ---Hour--- */
    if(regs[DS3231_REG_HOURS] & DS3231_BIT_12H) {
        /* 12H-Stundenformat */
        if(hour >= 12) {
            /* Nachmittag */
            /* nAM Bit setzen */
            regs[DS3231_REG_HOURS] |= DS3231_BIT_nAM;
            hour -= 12;
        }
        else {
            /* Vormittag  */
            /* nAM Bit zurücksetzen */
            regs[DS3231_REG_HOURS] &= ~DS3231_BIT_nAM;
        }
        regs[DS3231_REG_HOURS] &= ~0x1f;
        regs[DS3231_REG_HOURS] |= bin2bcd((u8)(hour) & 0x1f);
    }
    else {
        /* 24H-Stundenformat */
        regs[DS3231_REG_HOURS] &= ~0x3f;
        regs[DS3231_REG_HOURS] |= bin2bcd((u8)(hour) & 0x3f);
    }

    /* ---Minute--- */
    regs[DS3231_REG_MINUTES] = bin2bcd((u8)(date->tm_min));

    /* ---Seconds--- */
    regs[DS3231_REG_SECONDS] = bin2bcd((u8)(date->tm_sec));

    mutex_lock(&i2c_lock);
    ds3231_write_block_data(DS3231_REG_SECONDS, sizeof(regs), regs);
    mutex_unlock(&i2c_lock);
    if(ret < 0) {
        return ret;
    }

    return 0;
}


/*
 * Ueberpruefe ein Datum auf Korrektheit
 */
static u32 ds3231_check_date(const struct rtc_time *date)
{
    unsigned int curr_year = date->tm_year + 1900;

    if(date->tm_sec < 0 || date->tm_sec > 59) {
        goto check_fail;
    }

    if(date->tm_min < 0 || date->tm_min > 59) {
        goto check_fail;
    }

    if(date->tm_hour < 0 || date->tm_hour > 23) {
        goto check_fail;
    }

    if(date->tm_year < 100 || date->tm_year > 299) {
        goto check_fail;
    }

    switch(date->tm_mon) {
        case 0:
        case 2:
        case 4:
        case 6:
        case 7:
        case 9:
        case 11:
            if(date->tm_mday < 1 || date->tm_mday > 31) {
                goto check_fail;
            }
            break;
        case 3:
        case 5:
        case 8:
        case 10:
            if(date->tm_mday < 1 || date->tm_mday > 30) {
                goto check_fail;
            }
            break;
        case 1:
            if((curr_year % 4 == 0 && curr_year % 100 != 0) || (curr_year % 400 == 0)) {
                if(date->tm_mday < 1 || date->tm_mday > 29) {
                    goto check_fail;
                }
             }
             else {
                if(date->tm_mday < 1 || date->tm_mday > 28) {
                    goto check_fail;
                }
            }
            break;
        default:
            goto check_fail;
    }

    /* Datum ist gültig. */
    return 0;

check_fail:
    printk("DS3231_drv: Ungültiges Datum %02d-%02d-%04d %02d:%02d:%02d",
           date->tm_mday, date->tm_mon, date->tm_year + 1900,
           date->tm_hour, date->tm_min, date->tm_sec);
    return -EINVAL;
}


/* --------------------------------------------------------------------------------------------------------
    Callback-Funktionen für Device-Datei
   --------------------------------------------------------------------------------------------------------*/


/*
 * Wird beim Öffnen der Datei dev/ds3231 aufgerufen
 */
static int ds3231_dev_open(struct inode *inode, struct file *file) 
{
    printk("DS3231-drv: ds3231-Device-Datei wurde aufgerufen\n");
    return 0;
}


/*
 * Wird beim Schliessen der Datei dev/ds3231 aufgerufen
 */
static int ds3231_dev_close(struct inode *inode, struct file *file) 
{
    printk("DS3231-drv: Device-Datei geschlossen\n");
    return 0;
}


/*
 * Wird zum Lesen aufgerufen
 */
static int ds3231_dev_read(struct file *file, char __user *buf, size_t count, loff_t *offset) 
{
    size_t written = 0;
    char date_str[64];
    struct rtc_time date;

    printk("DS3231_drv: ds3231_dev_read aufgerufen\n");
    if(*offset != 0) {
        // End-Of-File -> Daten wurden bereits gelesen
        *offset = 0;
        return 0;
    }

    /* Lese RTC Daten von DS3231 */
    if(ds3231_read_date(&date) < 0) {
        return -EIO;
    }

    /* Die Datums-Werte in den String packen. */
    scnprintf(date_str , sizeof(date_str), "%02d.%02d.%04d %02d:%02d:%02d\n",
              date.tm_mday, date.tm_mon + 1, date.tm_year + 1900,
              date.tm_hour, date.tm_min, date.tm_sec);
    written = strlen(date_str);

    if(written > count) {
        printk("ds3231_drv: Benutzer-Buffer (%d) zu klein. Es werden %d bytes benötigt\n", count, written);
        written = count;
    }

    if(copy_to_user(buf, date_str, written)) {
        return -EFAULT;
    }

    *offset = 1;
    return written;
}


/*
 * Wird zum Schreiben aufgerufen
 */
static ssize_t ds3231_dev_write(struct file *file, const char __user *buf, size_t count, loff_t *offset) 
{
    char date_str[count];
    struct rtc_time date;
    s32 ret = 0;

    printk("DS3231_drv: ds3231_dev_write aufgerufen\n");

    if(count>20) {
        printk("DS3231_drv: Argument zu Lang\n");
        return -EINVAL;
    }

    if(copy_from_user(date_str, buf, count) != 0) {
        printk("DS3231_drv: Daten konnten nicht in den Kernel Space kopiert werden.\n");
        return -EINVAL;
    }

    if(!(date_str[4]  == '-' && date_str[7]  == '-' && date_str[10] == ' ' &&
         date_str[13] == ':' && date_str[16] == ':')) {
        printk("DS3231_drv: Falsche Formatierung des Datums\n");
        return -EINVAL;
    }

    /* Werte aus dem String lesen */
    sscanf(date_str, "%04d-%02d-%d %d:%d:%d",
        &date.tm_year, &date.tm_mon, &date.tm_mday,
        &date.tm_hour, &date.tm_min, &date.tm_sec);

    /* Wert für das Jahr anpassen */
    date.tm_mon--;
    date.tm_year -= 1900;

    /* Werte Prüfen und in Register schreiben */
    ret = ds3231_check_date(&date);
    if(ret != 0) {
        printk("DS3231_drv: Ungültiges Datum (error = %d).\n", ret);
        return ret;
    }

    ret = ds3231_write_date(&date);
    if(ret != 0) {
        printk("DS3231_drv: Datum konnte nicht geschrieben werden (error = %d).\n", ret);
        return ret;
    }

    return count;
}


/*
 * ioctl (input output control) verarbeitet Befehle
 */
static long ds3231_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg) 
{
        struct rtc_time date;
    s32 ret;

    printk("DS3231_drv: ioctl aufgerufen, cmd=0x%04x\n", cmd);

    switch(cmd) {
        case RTC_RD_TIME:
            memset(&date,0,sizeof(struct rtc_time));
            if(ds3231_read_date(&date) < 0) {
                return -EIO;
            }
            if(copy_to_user((void __user*)arg, &date, sizeof(struct rtc_time)) != 0) {
                return -EINVAL;
            }
            break;

        case RTC_SET_TIME:
            if(copy_from_user(&date, (struct rtc_time*)arg, sizeof(struct rtc_time)) != 0) {
                return -EINVAL;
            }

            /* Werte Prüfen und in Register schreiben */
            ret = ds3231_check_date(&date);
            if(ret != 0) {
                printk("DS3231_drv: Ungültiges Datum (error = %d)\n", ret);
                return ret;
            }

            ret = ds3231_write_date(&date);
            if(ret != 0) {
                printk("DS3231_drv: Datum konnte nicht geschrieben werden (error = %d)\n", ret);
                return ret;
            }
            break;

        /*
         * Update Interrupt Enabled.
         * Wird für hwclock benötigt.
         */
        case RTC_UIE_ON:
        case RTC_UIE_OFF:
            printk("DS3231_drv: UIE %s Befehl empfangen (0x%04x)\n",
                   (cmd == RTC_UIE_ON)?"ON":"OFF", cmd);
            break;

        default:
            printk("DS3231_drv: Unbekannter ioctl Befehl: 0x%04x \n", cmd);
            return -1;
    }

    return 0;
}


/*
 * Struktur für Registrierung der Callback-Funktionen
 */
static struct file_operations ds3231_fops = {
        .owner          = THIS_MODULE,
        .llseek         = no_llseek,
        .read           = ds3231_dev_read,
        .write          = ds3231_dev_write,
        .unlocked_ioctl = ds3231_dev_ioctl,
        .open           = ds3231_dev_open,
        .release        = ds3231_dev_close
};


/* Erste Geraetenummer */
static dev_t ds3231_first_dev;
/* Character Device Structure */
static struct cdev ds3231_cdev;
/* Geraeteklasse */
static struct class *ds3231_device_class;


/*
 * Initialisierung des Treibers und Devices.
 *
 * Diese Funktion wird von Linux-Kernel aufgerufen, aber erst nachdem ein zum
 * Treiber passende Device-Information gefunden wurde. Innerhalb der Funktion
 * wird der Treiber und das Device initialisiert.
 */
static int ds3231_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
        int ret;
        s32 reg0, reg1;
        u8 reg_cnt, reg_sts;

        printk("DS3231_drv: ds3231_probe called\n");

        /*
         * Control und Status Register auslesen.
         */
        reg0 = i2c_smbus_read_byte_data(client, DS3231_REG_CONTROL);
        reg1 = i2c_smbus_read_byte_data(client, DS3231_REG_STATUS);
        if(reg0 < 0 || reg1 < 0) {
                printk("DS3231_drv: Fehler beim Lesen von Control oder Status Register.\n");
                return -ENODEV;
        }
        reg_cnt = (u8)reg0;
        reg_sts = (u8)reg1;
        printk("DS3231_drv: Control: 0x%02X, Status: 0x%02X\n", reg_cnt, reg_sts);

        /*
         * Prüfen ob der Oscilattor abgeschaltet ist, falls ja, muss dieser
         * eingeschltet werden (damit die Zeit läuft).
         */
        if (reg_cnt & DS3231_BIT_nEOSC) {
                printk("DS3231_drv: Oscilator einschalten\n");
                reg_cnt &= ~DS3231_BIT_nEOSC;
        }

        printk("DS3231_drv: Interrupt und Alarms abschalten\n");
        reg_cnt &= ~(DS3231_BIT_INTCN | DS3231_BIT_A2IE | DS3231_BIT_A1IE);

        /* Control-Register setzen */
        i2c_smbus_write_byte_data(client, DS3231_REG_CONTROL, reg_cnt);

        /*
         * Prüfe Oscilator zustand. Falls Fehler vorhanden, wird das Fehlerflag
         * zurückgesetzt.
         */
        if (reg_sts & DS3231_BIT_OSF) {
                reg_sts &= ~DS3231_BIT_OSF;
                i2c_smbus_write_byte_data(client, DS3231_REG_STATUS, reg_sts);
                printk("DS3231_drv: Oscilator Stop Flag (OSF) zurückgesetzt.\n");
        }

        /*
         * chdev initialisieren
         * Es wird eine Zeichenorientierte Geraetedatei in dev angelegt
         */

        /* Geraetenummer allozieren */
        ret = alloc_chrdev_region(&ds3231_first_dev, 0, 1, "ds3231");
        if(ret < 0) {
            printk(KERN_ALERT "DS3231_drv: Device-Datei konnte nicht registriert werden (error = %d)\n", ret);
            return ret;
        }

        /*
         * Geraetedatei initialisieren
         */
        cdev_init(&ds3231_cdev, &ds3231_fops);
        ret = cdev_add(&ds3231_cdev, ds3231_first_dev, 1);
        if(ret < 0) {
            printk(KERN_ALERT "DS3231_drv: Device-Datei konnte nicht initialisiert werden (error = %d)\n", ret);
            goto unreg_chrdev;
        }

        /*
         * Im sysfs registrieren, damit Device-Datei automatisch angelegt wird
         */
        ds3231_device_class = class_create(THIS_MODULE, "chardev");
        if(ds3231_device_class == NULL) {
            printk(KERN_ALERT "DS3231_drv: Class konnte nicht erstellt werden\n");
            goto cleanup_cdev;
        }

        if(device_create(ds3231_device_class, NULL, ds3231_first_dev, NULL, "ds3231") == NULL) {
            printk(KERN_ALERT "DS3231_drv: Device konnte nicht erstellt werden\n");
            goto cleanup_chrdev_class;
        }

        /* DS3231 erfolgreich initialisiert */
        return 0;

        /* Resourcen freigeben */
        cleanup_chrdev_class:
            class_destroy(ds3231_device_class);
        cleanup_cdev:
            cdev_del(&ds3231_cdev);
        unreg_chrdev:
            unregister_chrdev_region(ds3231_first_dev, 1);
            return -EIO;
}


/*
 * Freigebe der Resourcen.
 *
 * Diese Funktion wird beim Entfernen des Treibers oder Gerätes
 * von Linux-Kernel aufgerufen. Hier sollten die Resourcen, welche
 * in der "ds3231_probe()" Funktion angefordert wurden, freigegeben.
 */
static int ds3231_remove(struct i2c_client *client)
{
        printk("DS3231_drv: ds3231_remove called\n");
        device_destroy(ds3231_device_class, ds3231_first_dev);
        class_destroy(ds3231_device_class);
        cdev_del(&ds3231_cdev);
        unregister_chrdev_region(ds3231_first_dev, 1);
        return 0;
}


/*
 * Device-Id. Wird für die Zuordnung des Treibers zum Gerät benötigt.
 * Das für den Treiber passendes Gerät mit der hier definierten Id wird
 * bei der Initialisierung des Moduls hinzugefügt.
 */
static const struct i2c_device_id ds3231_dev_id[] = {
        { "ds3231_drv", 0 },
        { }
};
MODULE_DEVICE_TABLE(i2c, ds3231_dev_id);


/*
 * I2C Treiber-Struktur. Wird für die Registrierung des Treibers im
 * Linux-Kernel benötigt.
 */
static struct i2c_driver ds3231_driver = {
        .driver = {
                .owner = THIS_MODULE,
                .name  = "ds3231_drv",
        },
        .id_table = ds3231_dev_id,
        .probe    = ds3231_probe,
        .remove   = ds3231_remove,
};


/*
 * Initialisierungsroutine des Kernel-Modules.
 *
 * Wird beim Laden des Moduls aufgerufen. Innerhalb der Funktion
 * wird das neue Device (DS3231) registriert und der I2C Treiber
 * hinzugefügt.
 */
static int __init ds3231_module_init(void)
{
        int ret;
        struct i2c_adapter *adapter;

        /*
         * Normaleweise werden die Informationen bezüglich der verbauten
         * Geräten (Devices) während der Kernel-Initialisierung mittels
         * eines Device-Trees Eintrages definiert. Anhand dieser Informationen
         * sucht der Kernel einen passenden Treiber für das Gerät (i2c_device_id).
         * In unserem Fall müssen die Informationen nachträglich definiert
         * und hinzugefügt werden.
         */
        const struct i2c_board_info info = {
                I2C_BOARD_INFO("ds3231_drv", 0x68)
        };

        printk("DS3231_drv: ds3231_module_init aufgerufen\n");

        ds3231_client = NULL;
        adapter = i2c_get_adapter(1);
        if(adapter == NULL) {
                printk("DS3231_drv: I2C Adapter nicht gefunden\n");
                return -ENODEV;
        }

        /* Neues I2C Device registrieren */
        ds3231_client = i2c_new_device(adapter, &info);
        if(ds3231_client == NULL) {
                printk("DS3231_drv: I2C Client: Registrierung fehlgeschlagen\n");
                return -ENODEV;
        }

        /* Treiber registrieren */
        ret = i2c_add_driver(&ds3231_driver);
        if(ret < 0) {
                printk("DS3231_drv: Treiber konnte nicht hinzugefügt werden (errorn = %d)\n", ret);
                i2c_unregister_device(ds3231_client);
                ds3231_client = NULL;
        }
        return ret;
}
module_init(ds3231_module_init);


/*
 * Aufräumroutine des Kernel-Modules.
 *
 * Wird beim Enladen des Moduls aufgerufen. Innerhalb der Funktion
 * werden alle Resourcen wieder freigegeben.
 */
static void __exit ds3231_module_exit(void)
{
        printk("DS3231_drv: ds3231_module_exit aufgerufen\n");
        if(ds3231_client != NULL) {
                i2c_del_driver(&ds3231_driver);
                i2c_unregister_device(ds3231_client);
        }
}
module_exit(ds3231_module_exit);


/* Module-Informationen. */
MODULE_AUTHOR("Pascal Otto <uk058821@student.uni-kassel.de>");
MODULE_DESCRIPTION("DS3231 - Real Time Clock Driver for System Ürogramming SS19");
MODULE_LICENSE("GPL");
