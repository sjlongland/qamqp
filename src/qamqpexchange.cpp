#include <QEventLoop>
#include <QDataStream>
#include <QTimer>
#include <QDebug>

#include "qamqpexchange.h"
#include "qamqpexchange_p.h"
#include "qamqpqueue.h"
#include "qamqpglobal.h"
#include "qamqpclient.h"

QString QAmqpExchangePrivate::typeToString(QAmqpExchange::ExchangeType type)
{
    switch (type) {
    case QAmqpExchange::Direct: return QLatin1String("direct");
    case QAmqpExchange::FanOut: return QLatin1String("fanout");
    case QAmqpExchange::Topic: return QLatin1String("topic");
    case QAmqpExchange::Headers: return QLatin1String("headers");
    }

    return QLatin1String("direct");
}

QAmqpExchangePrivate::QAmqpExchangePrivate(QAmqpExchange *q)
    : QAmqpChannelPrivate(q),
      delayedDeclare(false),
      exchangeState(EX_CLOSED),
      nextDeliveryTag(0)
{
}

void QAmqpExchangePrivate::declare()
{
    if (exchangeState != EX_UNDECLARED) {
        qAmqpDebug() << "Exchange" << name << "in state" << exchangeState;
        if (exchangeState != EX_DECLARING) {
            qAmqpDebug() << "Delaying declare of exchange "
                         << name;
            delayedDeclare = true;
        }
        return;
    }

    if (name.isEmpty()) {
        qAmqpDebug() << Q_FUNC_INFO << "attempting to declare an unnamed exchange, aborting...";
        return;
    }

    qAmqpDebug() << "Declaring exchange" << name;
    newState(EX_DECLARING);

    QAmqpMethodFrame frame(QAmqpFrame::Exchange, QAmqpExchangePrivate::miDeclare);
    frame.setChannel(channelNumber);

    QByteArray args;
    QDataStream stream(&args, QIODevice::WriteOnly);

    stream << qint16(0);    //reserved 1
    QAmqpFrame::writeAmqpField(stream, QAmqpMetaType::ShortString, name);
    QAmqpFrame::writeAmqpField(stream, QAmqpMetaType::ShortString, type);

    stream << qint8(options);
    QAmqpFrame::writeAmqpField(stream, QAmqpMetaType::Hash, arguments);

    frame.setArguments(args);
    sendFrame(frame);
    delayedDeclare = false;
}

bool QAmqpExchangePrivate::_q_method(const QAmqpMethodFrame &frame)
{
    Q_Q(QAmqpExchange);
    if (QAmqpChannelPrivate::_q_method(frame))
        return true;

    if (frame.methodClass() == QAmqpFrame::Basic) {
        switch (frame.id()) {
        case bmAck:
        case bmNack:
            handleAckOrNack(frame);
            break;
        case bmReturn: basicReturn(frame); break;

        default:
            break;
        }

        return true;
    }

    if (frame.methodClass() == QAmqpFrame::Confirm) {
        if (frame.id() == cmConfirmOk) {
            Q_EMIT q->confirmsEnabled();
            return true;
        }
    }

    if (frame.methodClass() == QAmqpFrame::Exchange) {
        switch (frame.id()) {
        case miDeclareOk: declareOk(frame); break;
        case miDeleteOk: deleteOk(frame); break;

        default:
            break;
        }

        return true;
    }

    return false;
}

void QAmqpExchangePrivate::declareOk(const QAmqpMethodFrame &frame)
{
    Q_UNUSED(frame)

    Q_Q(QAmqpExchange);
    qAmqpDebug() << "declared exchange: " << name;
    newState(EX_DECLARED);
    Q_EMIT q->declared();
}

void QAmqpExchangePrivate::deleteOk(const QAmqpMethodFrame &frame)
{
    Q_UNUSED(frame)

    Q_Q(QAmqpExchange);
    qAmqpDebug() << "deleted exchange: " << name;
    newState(EX_UNDECLARED);
    Q_EMIT q->removed();
}

void QAmqpExchangePrivate::_q_disconnected()
{
    QAmqpChannelPrivate::_q_disconnected();
    qAmqpDebug() << "exchange " << name << " disconnected";
    delayedDeclare = false;
    newState(EX_CLOSED);
}

void QAmqpExchangePrivate::basicReturn(const QAmqpMethodFrame &frame)
{
    Q_Q(QAmqpExchange);
    QByteArray data = frame.arguments();
    QDataStream stream(&data, QIODevice::ReadOnly);

    quint16 replyCode;
    stream >> replyCode;
    QString replyText = QAmqpFrame::readAmqpField(stream, QAmqpMetaType::ShortString).toString();
    QString exchangeName = QAmqpFrame::readAmqpField(stream, QAmqpMetaType::ShortString).toString();
    QString routingKey = QAmqpFrame::readAmqpField(stream, QAmqpMetaType::ShortString).toString();

    QAMQP::Error checkError = static_cast<QAMQP::Error>(replyCode);
    if (checkError != QAMQP::NoError) {
        error = checkError;
        errorString = qPrintable(replyText);
        Q_EMIT q->error(error);
    }

    qAmqpDebug(">> replyCode: %d", replyCode);
    qAmqpDebug(">> replyText: %s", qPrintable(replyText));
    qAmqpDebug(">> exchangeName: %s", qPrintable(exchangeName));
    qAmqpDebug(">> routingKey: %s", qPrintable(routingKey));
}

void QAmqpExchangePrivate::handleAckOrNack(const QAmqpMethodFrame &frame)
{
    Q_Q(QAmqpExchange);
    QByteArray data = frame.arguments();
    QDataStream stream(&data, QIODevice::ReadOnly);

    qlonglong deliveryTag =
        QAmqpFrame::readAmqpField(stream, QAmqpMetaType::LongLongUint).toLongLong();
    bool multiple = QAmqpFrame::readAmqpField(stream, QAmqpMetaType::Boolean).toBool();
    if (frame.id() == QAmqpExchangePrivate::bmAck) {
        if (deliveryTag == 0) {
            unconfirmedDeliveryTags.clear();
        } else {
            int idx = unconfirmedDeliveryTags.indexOf(deliveryTag);
            if (idx == -1) {
                return;
            }

            if (multiple) {
                unconfirmedDeliveryTags.remove(0, idx + 1);
            } else {
                unconfirmedDeliveryTags.remove(idx);
            }
        }

        if (unconfirmedDeliveryTags.isEmpty())
            Q_EMIT q->allMessagesDelivered();

    } else {
        qAmqpDebug() << "nacked(" << deliveryTag << "), multiple=" << multiple;
    }
}

/*! Report and change state. */
void QAmqpExchangePrivate::newState(ExchangeState state)
{
    qAmqpDebug() << "Exchange state: "
                 << exchangeState
                 << " -> "
                 << state;
    exchangeState = state;
}

void QAmqpExchangePrivate::newState(ChannelState state)
{
    QAmqpChannelPrivate::newState(state);
    if (state == QAmqpChannelPrivate::CH_CLOSED)
        newState(EX_CLOSED);
}

QDebug operator<<(QDebug dbg, QAmqpExchangePrivate::ExchangeState s)
{
    switch(s) {
        case QAmqpExchangePrivate::EX_CLOSED:
            dbg << "EX_CLOSED";
            break;
        case QAmqpExchangePrivate::EX_UNDECLARED:
            dbg << "EX_UNDECLARED";
            break;
        case QAmqpExchangePrivate::EX_DECLARING:
            dbg << "EX_DECLARING";
            break;
        case QAmqpExchangePrivate::EX_DECLARED:
            dbg << "EX_DECLARED";
            break;
        case QAmqpExchangePrivate::EX_REMOVING:
            dbg << "EX_REMOVING";
            break;
        default:
            dbg << "EX_????";
    }
    return dbg;
}

//////////////////////////////////////////////////////////////////////////

QAmqpExchange::QAmqpExchange(int channelNumber, QAmqpClient *parent)
    : QAmqpChannel(new QAmqpExchangePrivate(this), parent)
{
    Q_D(QAmqpExchange);
    d->init(channelNumber, parent);
}

QAmqpExchange::~QAmqpExchange()
{
}

void QAmqpExchange::channelOpened()
{
    Q_D(QAmqpExchange);
    qAmqpDebug() << "Channel open";

    if (name().isEmpty()) {
        /* Nameless exchange, we should consider this declared by default */
        qAmqpDebug() << "Automatically declaring built-in exchange: \"\"";
        d->newState(QAmqpExchangePrivate::EX_DECLARED);
        Q_EMIT declared();
        return;
    } else {
        qAmqpDebug() << "Exchange" << name() << "entering undeclared state.";
        d->newState(QAmqpExchangePrivate::EX_UNDECLARED);
    }

    if (d->delayedDeclare)
        d->declare();
    else
        qAmqpDebug() << "No delayed declare pending for" << name();
}

void QAmqpExchange::channelClosed()
{
    Q_D(QAmqpExchange);
    qAmqpDebug() << "Channel closed";
    d->newState(QAmqpExchangePrivate::EX_CLOSED);
}

QAmqpExchange::ExchangeOptions QAmqpExchange::options() const
{
    Q_D(const QAmqpExchange);
    return d->options;
}

QString QAmqpExchange::type() const
{
    Q_D(const QAmqpExchange);
    return d->type;
}

bool QAmqpExchange::isDeclared() const
{
    Q_D(const QAmqpExchange);
    return (d->exchangeState == QAmqpExchangePrivate::EX_DECLARED);
}

void QAmqpExchange::declare(ExchangeType type, ExchangeOptions options, const QAmqpTable &args)
{
    declare(QAmqpExchangePrivate::typeToString(type), options, args);
}

void QAmqpExchange::declare(const QString &type, ExchangeOptions options, const QAmqpTable &args)
{
    Q_D(QAmqpExchange);
    d->newState(QAmqpExchangePrivate::EX_DECLARING);
    d->type = type;
    d->options = options;
    d->arguments = args;
    d->declare();
}

void QAmqpExchange::remove(int options)
{
    Q_D(QAmqpExchange);
    if (d->exchangeState != QAmqpExchangePrivate::EX_DECLARED) {
        /* TODO: should we tell the caller about this? */
        qAmqpDebug()    << Q_FUNC_INFO
                        << "remove of exchange not in \"declared\" state";
        d->delayedDeclare = false;
        return;
    }

    QAmqpMethodFrame frame(QAmqpFrame::Exchange, QAmqpExchangePrivate::miDelete);
    frame.setChannel(d->channelNumber);

    QByteArray arguments;
    QDataStream stream(&arguments, QIODevice::WriteOnly);

    stream << qint16(0);    //reserved 1
    QAmqpFrame::writeAmqpField(stream, QAmqpMetaType::ShortString, d->name);
    stream << qint8(options);

    frame.setArguments(arguments);
    d->sendFrame(frame);
}

void QAmqpExchange::publish(const QString &message, const QString &routingKey,
                            const QAmqpMessage::PropertyHash &properties, int publishOptions)
{
    publish(message.toUtf8(), routingKey, QLatin1String("text.plain"),
            QAmqpTable(), properties, publishOptions);
}

void QAmqpExchange::publish(const QByteArray &message, const QString &routingKey,
                            const QString &mimeType, const QAmqpMessage::PropertyHash &properties,
                            int publishOptions)
{
    publish(message, routingKey, mimeType, QAmqpTable(), properties, publishOptions);
}

void QAmqpExchange::publish(const QByteArray &message, const QString &routingKey,
                            const QString &mimeType, const QAmqpTable &headers,
                            const QAmqpMessage::PropertyHash &properties, int publishOptions)
{
    Q_D(QAmqpExchange);
    if (d->exchangeState != QAmqpExchangePrivate::EX_DECLARED) {
        qAmqpDebug()    << Q_FUNC_INFO
                        << "Attempted to publish to exchange not yet declared.";
        return;
    }

    if (d->nextDeliveryTag > 0) {
        d->unconfirmedDeliveryTags.append(d->nextDeliveryTag);
        d->nextDeliveryTag++;
    }

    QAmqpMethodFrame frame(QAmqpFrame::Basic, QAmqpExchangePrivate::bmPublish);
    frame.setChannel(d->channelNumber);

    QByteArray arguments;
    QDataStream out(&arguments, QIODevice::WriteOnly);

    out << qint16(0);   //reserved 1
    QAmqpFrame::writeAmqpField(out, QAmqpMetaType::ShortString, d->name);
    QAmqpFrame::writeAmqpField(out, QAmqpMetaType::ShortString, routingKey);
    out << qint8(publishOptions);

    frame.setArguments(arguments);
    d->sendFrame(frame);

    QAmqpContentFrame content(QAmqpFrame::Basic);
    content.setChannel(d->channelNumber);
    content.setProperty(QAmqpMessage::ContentType, mimeType);
    content.setProperty(QAmqpMessage::ContentEncoding, "utf-8");
    content.setProperty(QAmqpMessage::Headers, headers);
    content.setProperty(QAmqpMessage::MessageId, "0");

    QAmqpMessage::PropertyHash::ConstIterator it;
    QAmqpMessage::PropertyHash::ConstIterator itEnd = properties.constEnd();
    for (it = properties.constBegin(); it != itEnd; ++it)
        content.setProperty(it.key(), it.value());
    content.setBodySize(message.size());
    d->sendFrame(content);

    int fullSize = message.size();
    for (int sent = 0; sent < fullSize; sent += (d->client->frameMax() - 7)) {
        QAmqpContentBodyFrame body;
        QByteArray partition = message.mid(sent, (d->client->frameMax() - 7));
        body.setChannel(d->channelNumber);
        body.setBody(partition);
        d->sendFrame(body);
    }
}

void QAmqpExchange::enableConfirms(bool noWait)
{
    Q_D(QAmqpExchange);
    if (d->exchangeState != QAmqpExchangePrivate::EX_DECLARED) {
        qAmqpDebug()    << Q_FUNC_INFO
                        << "Attempted to enable confirms on exchange "
                           "not yet declared.";
        return;
    }

    QAmqpMethodFrame frame(QAmqpFrame::Confirm, QAmqpExchangePrivate::cmConfirm);
    frame.setChannel(d->channelNumber);

    QByteArray arguments;
    QDataStream stream(&arguments, QIODevice::WriteOnly);
    stream << qint8(noWait ? 1 : 0);

    frame.setArguments(arguments);
    d->sendFrame(frame);

    // for tracking acks and nacks
    if (d->nextDeliveryTag == 0) d->nextDeliveryTag = 1;
}

bool QAmqpExchange::waitForConfirms(int msecs)
{
    Q_D(QAmqpExchange);

    QEventLoop loop;
    connect(this, SIGNAL(allMessagesDelivered()), &loop, SLOT(quit()));
    QTimer::singleShot(msecs, &loop, SLOT(quit()));
    loop.exec();

    return (d->unconfirmedDeliveryTags.isEmpty());
}
